#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int load_segment(pde_t *pagetable, uint64 virtual_address,
                        struct inode *inode, uint file_offset,
                        uint byte_count);

// ELFの権限をPTEの権限ビットに変換する。
int elf_flags_to_permissions(int flags)
{
    int permissions = 0;
    if (flags & 0x1)
        permissions = PTE_X;
    if (flags & 0x2)
        permissions |= PTE_W;
    return permissions;
}

//
// exec()システムコールの実装
//
int kexec(char *path, char **argv)
{
    char *path_character, *program_name;
    int i, program_header_offset;
    uint64 argc, memory_size = 0, stack_pointer;
    uint64 argument_addresses[MAXARG], stack_base;
    struct elfhdr elf_header;
    struct inode *executable_inode;
    struct proghdr program_header;
    pagetable_t pagetable = 0, old_pagetable;
    struct proc *process = myproc();

    begin_op();

    // 実行ファイルを開く。
    if ((executable_inode = namei(path)) == 0) {
        end_op();
        return -1;
    }
    ilock(executable_inode);

    // ELFヘッダを読み込む。
    if (readi(executable_inode, 0, (uint64)&elf_header, 0,
              sizeof(elf_header)) != sizeof(elf_header))
        goto exec_failed;

    // 本当にELFファイルか?
    if (elf_header.magic != ELF_MAGIC)
        goto exec_failed;

    if ((pagetable = proc_pagetable(process)) == 0)
        goto exec_failed;

    // プログラムをメモリに読み込む。
    for (i = 0, program_header_offset = elf_header.phoff;
         i < elf_header.phnum;
         i++, program_header_offset += sizeof(program_header)) {
        if (readi(executable_inode, 0, (uint64)&program_header,
                  program_header_offset, sizeof(program_header)) !=
            sizeof(program_header))
            goto exec_failed;
        if (program_header.type != ELF_PROG_LOAD)
            continue;
        if (program_header.memsz < program_header.filesz)
            goto exec_failed;
        if (program_header.vaddr + program_header.memsz < program_header.vaddr)
            goto exec_failed;
        if (program_header.vaddr % PGSIZE != 0)
            goto exec_failed;
        uint64 new_memory_size;
        if ((new_memory_size =
                 uvmalloc(pagetable, memory_size,
                          program_header.vaddr + program_header.memsz,
                          elf_flags_to_permissions(program_header.flags))) == 0)
            goto exec_failed;
        memory_size = new_memory_size;
        if (load_segment(pagetable, program_header.vaddr, executable_inode,
                         program_header.off, program_header.filesz) < 0)
            goto exec_failed;
    }
    iunlockput(executable_inode);
    end_op();
    executable_inode = 0;

    process = myproc();
    uint64 old_memory_size = process->memory_size;

    // 次のページ境界から何ページかを割り当てる。
    // 先頭ページをアクセス不能にしてスタックガードにする。
    // 残りをユーザスタックとして使う。
    memory_size = PGROUNDUP(memory_size);
    uint64 new_memory_size;
    if ((new_memory_size =
             uvmalloc(pagetable, memory_size,
                      memory_size + (USERSTACK + 1) * PGSIZE, PTE_W)) == 0)
        goto exec_failed;
    memory_size = new_memory_size;
    uvmclear(pagetable, memory_size - (USERSTACK + 1) * PGSIZE);
    stack_pointer = memory_size;
    stack_base = stack_pointer - USERSTACK * PGSIZE;

    // 引数文字列を新しいスタックに複写し、そのアドレスをustack[]に記録する。
    for (argc = 0; argv[argc]; argc++) {
        stack_pointer -= strlen(argv[argc]) + 1;
        stack_pointer -= stack_pointer % 16;
        if (stack_pointer < stack_base)
            goto exec_failed;
        if (copyout(pagetable, memory_size, stack_pointer, argv[argc],
                    strlen(argv[argc]) + 1) < 0)
            goto exec_failed;
        argument_addresses[argc] = stack_pointer;
    }
    argument_addresses[argc] = 0;

    // argv[]ポインタの配列argument_addresses[]をスタックに積む。
    stack_pointer -= (argc + 1) * sizeof(uint64);
    stack_pointer -= stack_pointer % 16;
    if (stack_pointer < stack_base)
        goto exec_failed;
    if (copyout(pagetable, memory_size, stack_pointer,
                (char *)argument_addresses,
                (argc + 1) * sizeof(uint64)) < 0)
        goto exec_failed;

    // a0とa1にはユーザmain(argc, argv)の引数が入る。
    // argcはシステムコールの戻り値として返され、
    // a0に入る。
    process->trapframe->a1 = stack_pointer;

    // デバッグ用にプログラム名を保存する。
    for (program_name = path_character = path;
         *path_character; path_character++)
        if (*path_character == '/')
            program_name = path_character + 1;
    safestrcpy(process->name, program_name, sizeof(process->name));

    // ユーザイメージを確定する。
    old_pagetable = process->pagetable;
    process->pagetable = pagetable;
    process->memory_size = memory_size;
    process->trapframe->epc = elf_header.entry;
    process->trapframe->sp = stack_pointer;
    proc_freepagetable(old_pagetable, old_memory_size);

    return argc; // main(argc, argv)の第1引数としてa0に入る

exec_failed:
    if (pagetable)
        proc_freepagetable(pagetable, memory_size);
    if (executable_inode) {
        iunlockput(executable_inode);
        end_op();
    }
    return -1;
}

// ELFプログラムセグメントを指定仮想アドレスへ読み込む。
// virtual_addressはページ境界に揃っていなければならない。
// 成功時は0、失敗時は-1を返す。
static int load_segment(pagetable_t pagetable, uint64 virtual_address,
                        struct inode *inode, uint file_offset,
                        uint byte_count)
{
    uint bytes_loaded, chunk_byte_count;
    uint64 physical_address;

    for (bytes_loaded = 0; bytes_loaded < byte_count;
         bytes_loaded += PGSIZE) {
        physical_address = walkaddr(pagetable,
                                    virtual_address + bytes_loaded);
        if (physical_address == 0)
            panic("loadseg: address should exist");
        if (byte_count - bytes_loaded < PGSIZE)
            chunk_byte_count = byte_count - bytes_loaded;
        else
            chunk_byte_count = PGSIZE;
        if (readi(inode, 0, physical_address,
                  file_offset + bytes_loaded, chunk_byte_count) !=
            chunk_byte_count)
            return -1;
    }

    return 0;
}
