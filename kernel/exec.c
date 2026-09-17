#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// ELFの権限をPTEの権限ビットに変換する。
int flags2perm(int flags)
{
    int perm = 0;
    if (flags & 0x1)
        perm = PTE_X;
    if (flags & 0x2)
        perm |= PTE_W;
    return perm;
}

//
// exec()システムコールの実装
//
int kexec(char *path, char **argv)
{
    char *s, *last;
    int i, off;
    uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    pagetable_t pagetable = 0, oldpagetable;
    struct proc *p = myproc();

    begin_op();

    // 実行ファイルを開く。
    if ((ip = namei(path)) == 0) {
        end_op();
        return -1;
    }
    ilock(ip);

    // ELFヘッダを読み込む。
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;

    // 本当にELFファイルか?
    if (elf.magic != ELF_MAGIC)
        goto bad;

    if ((pagetable = proc_pagetable(p)) == 0)
        goto bad;

    // プログラムをメモリに読み込む。
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            goto bad;
        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz)
            goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto bad;
        if (ph.vaddr % PGSIZE != 0)
            goto bad;
        uint64 sz1;
        if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz,
                            flags2perm(ph.flags))) == 0)
            goto bad;
        sz = sz1;
        if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
            goto bad;
    }
    iunlockput(ip);
    end_op();
    ip = 0;

    p = myproc();
    uint64 oldsz = p->sz;

    // 次のページ境界から何ページかを割り当てる。
    // 先頭ページをアクセス不能にしてスタックガードにする。
    // 残りをユーザスタックとして使う。
    sz = PGROUNDUP(sz);
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) ==
        0)
        goto bad;
    sz = sz1;
    uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
    sp = sz;
    stackbase = sp - USERSTACK * PGSIZE;

    // 引数文字列を新しいスタックに複写し、そのアドレスをustack[]に記録する。
    for (argc = 0; argv[argc]; argc++) {
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16; // riscvのspは16バイト境界に揃える必要がある
        if (sp < stackbase)
            goto bad;
        if (copyout(pagetable, sz, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
            goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;

    // argv[]ポインタの配列ustack[]の複製をスタックに積む。
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase)
        goto bad;
    if (copyout(pagetable, sz, sp, (char *)ustack,
                (argc + 1) * sizeof(uint64)) < 0)
        goto bad;

    // a0とa1にはユーザmain(argc, argv)の引数が入る。
    // argcはシステムコールの戻り値として返され、
    // a0に入る。
    p->trapframe->a1 = sp;

    // デバッグ用にプログラム名を保存する。
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    // ユーザイメージを確定する。
    oldpagetable = p->pagetable;
    p->pagetable = pagetable;
    p->sz = sz;
    p->trapframe->epc = elf.entry; // 初期プログラムカウンタ = ulib.c:start()
    p->trapframe->sp = sp;         // 初期スタックポインタ
    proc_freepagetable(oldpagetable, oldsz);

    return argc; // main(argc, argv)の第1引数としてa0に入る

bad:
    if (pagetable)
        proc_freepagetable(pagetable, sz);
    if (ip) {
        iunlockput(ip);
        end_op();
    }
    return -1;
}

// ELFプログラムセグメントを仮想アドレスvaのページテーブルに読み込む。
// vaはページ境界に揃っていなければならず、
// vaからva+szまでのページはあらかじめマップされていなければならない。
// 成功時は0、失敗時は-1を返す。
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip,
                   uint offset, uint sz)
{
    uint i, n;
    uint64 pa;

    for (i = 0; i < sz; i += PGSIZE) {
        pa = walkaddr(pagetable, va + i);
        if (pa == 0)
            panic("loadseg: address should exist");
        if (sz - i < PGSIZE)
            n = sz - i;
        else
            n = PGSIZE;
        if (readi(ip, 0, (uint64)pa, offset + i, n) != n)
            return -1;
    }

    return 0;
}
