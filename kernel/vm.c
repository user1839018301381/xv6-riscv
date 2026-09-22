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
    pagetable_t new_kernel_pagetable;

    new_kernel_pagetable = (pagetable_t)kalloc();
    memset(new_kernel_pagetable, 0, PGSIZE);

    // uartレジスタ
    kvmmap(new_kernel_pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio MMIOディスクインタフェース
    kvmmap(new_kernel_pagetable, VIRTIO0, VIRTIO0, PGSIZE,
           PTE_R | PTE_W);

    // PLIC
    kvmmap(new_kernel_pagetable, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // カーネルテキストを実行可能かつ読み取り専用でマップする。
    kvmmap(new_kernel_pagetable, KERNBASE, KERNBASE,
           (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // カーネルデータと使用する物理RAMをマップする。
    kvmmap(new_kernel_pagetable, (uint64)etext, (uint64)etext,
           PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // トラップの入口・出口で使うトランポリンを、
    // カーネルの最高仮想アドレスにマップする。
    kvmmap(new_kernel_pagetable, TRAMPOLINE, (uint64)trampoline,
           PGSIZE, PTE_R | PTE_X);

    // 各プロセスのカーネルスタックを割り当ててマップする。
    proc_mapstacks(new_kernel_pagetable);

    return new_kernel_pagetable;
}

// カーネルページテーブルにマッピングを追加する。
// 起動時にだけ使う。
// TLBのフラッシュやページングの有効化は行わない。
void kvmmap(pagetable_t pagetable, uint64 virtual_address,
            uint64 physical_address, uint64 byte_count, int permissions)
{
    if (mappages(pagetable, virtual_address, byte_count,
                 physical_address, permissions) != 0)
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
pte_t *walk(pagetable_t pagetable, uint64 va, int should_allocate)
{
    if (va >= MAXVA)
        panic("walk");

    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!should_allocate || (pagetable = (pde_t *)kalloc()) == 0)
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
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int permissions)
{
    uint64 current_va, last_va;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("mappages: va not aligned");

    if ((size % PGSIZE) != 0)
        panic("mappages: size not aligned");

    if (size == 0)
        panic("mappages: size");

    current_va = va;
    last_va = va + size - PGSIZE;
    for (;;) {
        if ((pte = walk(pagetable, current_va, 1)) == 0)
            return -1;
        if (*pte & PTE_V)
            panic("mappages: remap");
        *pte = PA2PTE(pa) | permissions | PTE_V;
        if (current_va == last_va)
            break;
        current_va += PGSIZE;
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

// vaから始まるpage_count個のマッピングを削除する。
// vaはページ境界に揃っていなければならない。
// マッピングが存在しなくてもよい。
// 物理メモリは任意で解放する。
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 page_count,
              int should_free_physical_pages)
{
    uint64 current_va;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (current_va = va; current_va < va + page_count * PGSIZE;
         current_va += PGSIZE) {
        if ((pte = walk(pagetable, current_va, 0)) ==
            0) // 葉のページテーブルエントリが割り当て済みか?
            continue;
        if ((*pte & PTE_V) == 0) // 物理ページが割り当て済みか?
            continue;
        if (should_free_physical_pages) {
            uint64 pa = PTE2PA(*pte);
            kfree((void *)pa);
        }
        *pte = 0;
    }
}

// old_sizeからnew_sizeまでプロセスを拡張するため、PTEと物理メモリを割り当てる。
// new_sizeはページ境界に揃っていなくてもよい。
// 新しいサイズ、またはエラー時に0を返す。
uint64 uvmalloc(pagetable_t pagetable, uint64 old_size, uint64 new_size,
                int extra_permissions)
{
    char *page;
    uint64 virtual_address;

    if (new_size < old_size)
        return old_size;

    old_size = PGROUNDUP(old_size);
    for (virtual_address = old_size; virtual_address < new_size;
         virtual_address += PGSIZE) {
        page = kalloc();
        if (page == 0) {
            uvmdealloc(pagetable, virtual_address, old_size);
            return 0;
        }
        memset(page, 0, PGSIZE);
        if (mappages(pagetable, virtual_address, PGSIZE, (uint64)page,
                     PTE_R | PTE_U | extra_permissions) != 0) {
            kfree(page);
            uvmdealloc(pagetable, virtual_address, old_size);
            return 0;
        }
    }
    return new_size;
}

// ユーザページを解放して、プロセスサイズをold_sizeからnew_sizeにする。
// サイズはページ境界に揃っていなくてもよい。
// old_sizeは実際のプロセスサイズより大きくてもよい。
uint64 uvmdealloc(pagetable_t pagetable, uint64 old_size, uint64 new_size)
{
    if (new_size >= old_size)
        return old_size;

    if (PGROUNDUP(new_size) < PGROUNDUP(old_size)) {
        int page_count = (PGROUNDUP(old_size) - PGROUNDUP(new_size)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(new_size), page_count, 1);
    }

    return new_size;
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
void uvmfree(pagetable_t pagetable, uint64 memory_size)
{
    if (memory_size > 0)
        uvmunmap(pagetable, 0, PGROUNDUP(memory_size) / PGSIZE, 1);
    freewalk(pagetable);
}

// 親プロセスのページテーブルを受け取り、そのメモリを子のページテーブルへ複写する。
// ページテーブルと物理メモリの両方を複写する。
// 成功時は0、失敗時は-1を返す。
// 失敗時には割り当て済みのページを解放する。
int uvmcopy(pagetable_t parent_pagetable, pagetable_t child_pagetable,
            uint64 memory_size)
{
    pte_t *pte;
    uint64 physical_address, virtual_address;
    uint flags;
    char *page;

    for (virtual_address = 0; virtual_address < memory_size;
         virtual_address += PGSIZE) {
        if ((pte = walk(parent_pagetable, virtual_address, 0)) == 0)
            continue; // ページテーブルエントリが割り当てられていない
        if ((*pte & PTE_V) == 0)
            continue; // 物理ページが割り当てられていない
        physical_address = PTE2PA(*pte);
        flags = PTE_FLAGS(*pte);
        if ((page = kalloc()) == 0)
            goto copy_failed;
        memmove(page, (char *)physical_address, PGSIZE);
        if (mappages(child_pagetable, virtual_address, PGSIZE,
                     (uint64)page, flags) != 0) {
            kfree(page);
            goto copy_failed;
        }
    }
    return 0;

copy_failed:
    uvmunmap(child_pagetable, 0, virtual_address / PGSIZE, 1);
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
// 指定ページテーブルでsourceからdestination_vaへbyte_countバイト複写する。
// 成功時は0、エラー時は-1を返す。
int copyout(pagetable_t pagetable, uint64 process_size,
            uint64 destination_va, char *source, uint64 byte_count)
{
    uint64 chunk_byte_count, page_va, page_pa;
    pte_t *pte;

    while (byte_count > 0) {
        page_va = PGROUNDDOWN(destination_va);
        if (page_va >= MAXVA)
            return -1;

        page_pa = walkaddr(pagetable, page_va);
        if (page_pa == 0) {
            if ((page_pa = vmfault(pagetable, process_size,
                                   page_va, 0)) == 0) {
                return -1;
            }
        }

        pte = walk(pagetable, page_va, 0);
        // 読み取り専用のユーザテキストページへのcopyoutを禁止する。
        if ((*pte & PTE_W) == 0)
            return -1;

        chunk_byte_count = PGSIZE - (destination_va - page_va);
        if (chunk_byte_count > byte_count)
            chunk_byte_count = byte_count;
        memmove((void *)(page_pa + (destination_va - page_va)), source,
                chunk_byte_count);

        byte_count -= chunk_byte_count;
        source += chunk_byte_count;
        destination_va = page_va + PGSIZE;
    }
    return 0;
}

// ユーザからカーネルへ複写する。
// 指定ページテーブルでsource_vaからdestinationへbyte_countバイト複写する。
// 成功時は0、エラー時は-1を返す。
int copyin(pagetable_t pagetable, uint64 process_size, char *destination,
           uint64 source_va, uint64 byte_count)
{
    uint64 chunk_byte_count, page_va, page_pa;

    while (byte_count > 0) {
        page_va = PGROUNDDOWN(source_va);
        page_pa = walkaddr(pagetable, page_va);
        if (page_pa == 0) {
            if ((page_pa = vmfault(pagetable, process_size, page_va, 1)) == 0) {
                return -1;
            }
        }
        chunk_byte_count = PGSIZE - (source_va - page_va);
        if (chunk_byte_count > byte_count)
            chunk_byte_count = byte_count;
        memmove(destination, (void *)(page_pa + (source_va - page_va)),
                chunk_byte_count);

        byte_count -= chunk_byte_count;
        destination += chunk_byte_count;
        source_va = page_va + PGSIZE;
    }
    return 0;
}

// ユーザからカーネルへNUL終端文字列を複写する。
// 指定ページテーブルでsource_vaからdestinationへNULまで複写する。
// 成功時は0、エラー時は-1を返す。
int copyinstr(pagetable_t pagetable, uint64 process_size, char *destination,
              uint64 source_va, uint64 max_length)
{
    uint64 chunk_byte_count, page_va, page_pa;
    int is_terminated = 0;

    while (!is_terminated && max_length > 0) {
        page_va = PGROUNDDOWN(source_va);
        page_pa = walkaddr(pagetable, page_va);
        if (page_pa == 0) {
            if ((page_pa = vmfault(pagetable, process_size, page_va, 1)) == 0) {
                return -1;
            }
        }
        chunk_byte_count = PGSIZE - (source_va - page_va);
        if (chunk_byte_count > max_length)
            chunk_byte_count = max_length;

        char *source_bytes = (char *)(page_pa + (source_va - page_va));
        while (chunk_byte_count > 0) {
            if (*source_bytes == '\0') {
                *destination = '\0';
                is_terminated = 1;
                break;
            } else {
                *destination = *source_bytes;
            }
            --chunk_byte_count;
            --max_length;
            source_bytes++;
            destination++;
        }

        source_va = page_va + PGSIZE;
    }
    if (is_terminated) {
        return 0;
    } else {
        return -1;
    }
}

// プロセスがsys_sbrk()で遅延割り当てされたページを参照している場合、
// ユーザメモリを割り当ててマップする。
// vaが無効または既にマップ済み、あるいは物理メモリ不足なら0を返し、
// 成功時は物理アドレスを返す。
uint64 vmfault(pagetable_t pagetable, uint64 process_size, uint64 va,
               int access_is_read)
{
    uint64 page_pa;

    if (va >= process_size)
        return 0;
    va = PGROUNDDOWN(va);
    if (is_mapped(pagetable, va)) {
        return 0;
    }
    page_pa = (uint64)kalloc();
    if (page_pa == 0)
        return 0;
    memset((void *)page_pa, 0, PGSIZE);
    if (mappages(pagetable, va, PGSIZE, page_pa,
                 PTE_W | PTE_U | PTE_R) != 0) {
        kfree((void *)page_pa);
        return 0;
    }
    return page_pa;
}

int is_mapped(pagetable_t pagetable, uint64 va)
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
