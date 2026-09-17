#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * カーネルのページテーブル。
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ldがカーネルコードの終端に設定する。

extern char trampoline[]; // trampoline.Sのコード

// カーネル用のダイレクトマップページテーブルを作る。
pagetable_t kvmmake(void)
{
    pagetable_t kpgtbl;

    kpgtbl = (pagetable_t)kalloc();
    memset(kpgtbl, 0, PGSIZE);

    // uartレジスタ
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio MMIOディスクインタフェース
    kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // PLIC
    kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // カーネルテキストを実行可能かつ読み取り専用でマップする。
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // カーネルデータと使用する物理RAMをマップする。
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
           PTE_R | PTE_W);

    // トラップの入口・出口で使うトランポリンを、
    // カーネルの最高仮想アドレスにマップする。
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 各プロセスのカーネルスタックを割り当ててマップする。
    proc_mapstacks(kpgtbl);

    return kpgtbl;
}

// カーネルページテーブルにマッピングを追加する。
// 起動時にだけ使う。
// TLBのフラッシュやページングの有効化は行わない。
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
    if (mappages(kpgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

// 全CPUで共有するkernel_pagetableを初期化する。
void kvminit(void) { kernel_pagetable = kvmmake(); }

// 現在のCPUのハードウェアページテーブルレジスタを
// カーネルのページテーブルに切り替え、ページングを有効にする。
void kvminithart()
{
    // ページテーブルメモリへの先行する書き込みが完了するのを待つ。
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));

    // TLBから古いエントリをフラッシュする。
    sfence_vma();
}

// ページテーブルpagetableで、仮想アドレスvaに対応するPTEのアドレスを返す。
// allocが0でなければ必要なページテーブルページを作成する。
//
// RISC-VのSv39方式ではページテーブルページが3階層ある。
// 1つのページテーブルページには512個の64ビットPTEがある。
// 64ビット仮想アドレスは5つのフィールドに分かれる:
//   39..63 -- 0でなければならない。
//   30..38 -- レベル2の添字、9ビット。
//   21..29 -- レベル1の添字、9ビット。
//   12..20 -- レベル0の添字、9ビット。
//    0..11 -- ページ内のバイトオフセット、12ビット。
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc)
{
    if (va >= MAXVA)
        panic("walk");

    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
                return 0;
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}

// 仮想アドレスを検索して物理アドレスを返す。
// マップされていなければ0を返す。
// ユーザページの検索にのみ使える。
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA)
        return 0;

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    pa = PTE2PA(*pte);
    return pa;
}

// vaから始まる仮想アドレス用に、paから始まる物理アドレスを参照するPTEを作る。
// vaとsizeはページ境界に揃っていなければならない。
// 成功時は0、walk()が必要なページテーブルページを割り当てられなければ-1を返す。
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
    uint64 a, last;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("mappages: va not aligned");

    if ((size % PGSIZE) != 0)
        panic("mappages: size not aligned");

    if (size == 0)
        panic("mappages: size");

    a = va;
    last = va + size - PGSIZE;
    for (;;) {
        if ((pte = walk(pagetable, a, 1)) == 0)
            return -1;
        if (*pte & PTE_V)
            panic("mappages: remap");
        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

// 空のユーザページテーブルを作る。
// メモリ不足なら0を返す。
pagetable_t uvmcreate()
{
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == 0)
        return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// vaから始まるnpages個のマッピングを削除する。vaはページ境界に揃っていなければならない。
// マッピングが存在しなくてもよい。
// 物理メモリは任意で解放する。
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        if ((pte = walk(pagetable, a, 0)) ==
            0) // 葉のページテーブルエントリが割り当て済みか?
            continue;
        if ((*pte & PTE_V) == 0) // 物理ページが割り当て済みか?
            continue;
        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            kfree((void *)pa);
        }
        *pte = 0;
    }
}

// oldszからnewszまでプロセスを拡張するため、PTEと物理メモリを割り当てる。
// newszはページ境界に揃っていなくてもよい。新しいサイズ、またはエラー時に0を返す。
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
    char *mem;
    uint64 a;

    if (newsz < oldsz)
        return oldsz;

    oldsz = PGROUNDUP(oldsz);
    for (a = oldsz; a < newsz; a += PGSIZE) {
        mem = kalloc();
        if (mem == 0) {
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
        memset(mem, 0, PGSIZE);
        if (mappages(pagetable, a, PGSIZE, (uint64)mem,
                     PTE_R | PTE_U | xperm) != 0) {
            kfree(mem);
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
    }
    return newsz;
}

// ユーザページを解放して、プロセスサイズをoldszからnewszにする。
// oldszとnewszはページ境界に揃っていなくてもよく、newszがoldszより小さい必要もない。
// oldszは実際のプロセスサイズより大きくてもよい。新しいプロセスサイズを返す。
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
    if (newsz >= oldsz)
        return oldsz;

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    return newsz;
}

// ページテーブルページを再帰的に解放する。
// 葉のマッピングはすべて事前に削除されていなければならない。
void freewalk(pagetable_t pagetable)
{
    // ページテーブルには2^9 = 512個のPTEがある。
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // このPTEは下位レベルのページテーブルを指している。
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    kfree((void *)pagetable);
}

// ユーザメモリページを解放し、
// その後でページテーブルページを解放する。
void uvmfree(pagetable_t pagetable, uint64 sz)
{
    if (sz > 0)
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    freewalk(pagetable);
}

// 親プロセスのページテーブルを受け取り、そのメモリを子のページテーブルへ複写する。
// ページテーブルと物理メモリの両方を複写する。
// 成功時は0、失敗時は-1を返す。
// 失敗時には割り当て済みのページを解放する。
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
    pte_t *pte;
    uint64 pa, i;
    uint flags;
    char *mem;

    for (i = 0; i < sz; i += PGSIZE) {
        if ((pte = walk(old, i, 0)) == 0)
            continue; // ページテーブルエントリが割り当てられていない
        if ((*pte & PTE_V) == 0)
            continue; // 物理ページが割り当てられていない
        pa = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        if ((mem = kalloc()) == 0)
            goto err;
        memmove(mem, (char *)pa, PGSIZE);
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            goto err;
        }
    }
    return 0;

err:
    uvmunmap(new, 0, i / PGSIZE, 1);
    return -1;
}

// PTEをユーザから無効にする。
// execがユーザスタックのガードページに使う。
void uvmclear(pagetable_t pagetable, uint64 va)
{
    pte_t *pte;

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        panic("uvmclear");
    *pte &= ~PTE_U;
}

// カーネルからユーザへ複写する。
// 指定ページテーブルでsrcから仮想アドレスdstvaへlenバイト複写する。
// 成功時は0、エラー時は-1を返す。
int copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src,
            uint64 len)
{
    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA)
            return -1;

        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, psz, va0, 0)) == 0) {
                return -1;
            }
        }

        pte = walk(pagetable, va0, 0);
        // 読み取り専用のユーザテキストページへのcopyoutを禁止する。
        if ((*pte & PTE_W) == 0)
            return -1;

        n = PGSIZE - (dstva - va0);
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// ユーザからカーネルへ複写する。
// 指定ページテーブルで仮想アドレスsrcvaからdstへlenバイト複写する。
// 成功時は0、エラー時は-1を返す。
int copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
           uint64 len)
{
    uint64 n, va0, pa0;

    while (len > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
                return -1;
            }
        }
        n = PGSIZE - (srcva - va0);
        if (n > len)
            n = len;
        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// ユーザからカーネルへNUL終端文字列を複写する。
// 指定ページテーブルで仮想アドレスsrcvaからdstへ、'\0'またはmaxまで複写する。
// 成功時は0、エラー時は-1を返す。
int copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
              uint64 max)
{
    uint64 n, va0, pa0;
    int got_null = 0;

    while (got_null == 0 && max > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, psz, va0, 1)) == 0) {
                return -1;
            }
        }
        n = PGSIZE - (srcva - va0);
        if (n > max)
            n = max;

        char *p = (char *)(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PGSIZE;
    }
    if (got_null) {
        return 0;
    } else {
        return -1;
    }
}

// プロセスがsys_sbrk()で遅延割り当てされたページを参照している場合、
// ユーザメモリを割り当ててマップする。
// vaが無効または既にマップ済み、あるいは物理メモリ不足なら0を返し、
// 成功時は物理アドレスを返す。
uint64 vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
    uint64 mem;

    if (va >= psz)
        return 0;
    va = PGROUNDDOWN(va);
    if (ismapped(pagetable, va)) {
        return 0;
    }
    mem = (uint64)kalloc();
    if (mem == 0)
        return 0;
    memset((void *)mem, 0, PGSIZE);
    if (mappages(pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
        kfree((void *)mem);
        return 0;
    }
    return mem;
}

int ismapped(pagetable_t pagetable, uint64 va)
{
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0) {
        return 0;
    }
    if (*pte & PTE_V) {
        return 1;
    }
    return 0;
}
