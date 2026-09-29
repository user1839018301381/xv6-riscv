#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int load_segment(pde_t *pagetable, uint64 virtual_address,
                        struct inode *inode, uint file_offset, uint byte_count);

struct user_stack {
    uint64 pointer;
    uint64 argv_address;
};

// ELFの権限をPTEの権限ビットに変換する。
int elf_flags_to_permissions(int flags)
{
    int permissions = 0;
    if (flags & ELF_PROG_FLAG_EXEC)
        permissions = PTE_X;
    if (flags & ELF_PROG_FLAG_WRITE)
        permissions |= PTE_W;
    return permissions;
}

static int load_elf_segments(pagetable_t pagetable,
                             struct inode *executable_inode,
                             struct elfhdr *elf_header, uint64 *memory_size)
{
    struct proghdr program_header;
    int program_header_offset = elf_header->phoff;

    for (int i = 0; i < elf_header->phnum;
         i++, program_header_offset += sizeof(program_header)) {
        if (readi(executable_inode, 0, (uint64)&program_header,
                  program_header_offset,
                  sizeof(program_header)) != sizeof(program_header))
            return -1;

        if (program_header.type != ELF_PROG_LOAD)
            continue;

        if (program_header.memsz < program_header.filesz)
            return -1;
        if (program_header.vaddr + program_header.memsz < program_header.vaddr)
            return -1;
        if (program_header.vaddr % PGSIZE != 0)
            return -1;

        uint64 new_memory_size =
            uvmalloc(pagetable, *memory_size,
                     program_header.vaddr + program_header.memsz,
                     elf_flags_to_permissions(program_header.flags));

        if (new_memory_size == 0)
            return -1;

        *memory_size = new_memory_size;

        if (load_segment(pagetable, program_header.vaddr, executable_inode,
                         program_header.off, program_header.filesz) < 0)
            return -1;
    }

    return 0;
}

// ガードページとユーザスタックを作り、引数を積む。成功時はargcを返す。
static int setup_user_stack(pagetable_t pagetable, uint64 *memory_size,
                            char **argv, struct user_stack *stack)
{
    uint64 argument_addresses[MAXARG];
    uint64 stack_base;

    *memory_size = PGROUNDUP(*memory_size);

    uint64 new_memory_size =
        uvmalloc(pagetable, *memory_size,
                 *memory_size + (USERSTACK + 1) * PGSIZE, PTE_W);

    if (new_memory_size == 0)
        return -1;

    *memory_size = new_memory_size;

    // スタック直下のページをユーザからアクセス不能にする。
    uvmclear(pagetable, *memory_size - (USERSTACK + 1) * PGSIZE);

    stack->pointer = *memory_size;
    stack_base = stack->pointer - USERSTACK * PGSIZE;

    int argc;
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG - 1)
            return -1;

        uint64 argument_length = strlen(argv[argc]) + 1;
        stack->pointer -= argument_length;

        // RISC-V ABIに合わせて16バイト境界に揃える。
        stack->pointer -= stack->pointer % 16;

        if (stack->pointer < stack_base)
            return -1;

        if (copyout(pagetable, *memory_size, stack->pointer, argv[argc],
                    argument_length) < 0)
            return -1;

        argument_addresses[argc] = stack->pointer;
    }

    argument_addresses[argc] = 0;

    stack->pointer -= (argc + 1) * sizeof(uint64);
    stack->pointer -= stack->pointer % 16;

    if (stack->pointer < stack_base)
        return -1;

    if (copyout(pagetable, *memory_size, stack->pointer,
                (char *)argument_addresses, (argc + 1) * sizeof(uint64)) < 0)
        return -1;

    stack->argv_address = stack->pointer;

    return argc;
}

//
// exec()システムコールの実装
//
int kexec(char *path, char **argv)
{
    struct proc *process = myproc();
    struct elfhdr elf_header;
    struct inode *executable_inode = 0;
    struct user_stack stack;
    pagetable_t pagetable = 0;
    pagetable_t old_pagetable;
    uint64 memory_size = 0;
    char *path_character;
    char *program_name;

    begin_op();

    if ((executable_inode = namei(path)) == 0) {
        end_op();
        return -1;
    }

    ilock(executable_inode);

    if (readi(executable_inode, 0, (uint64)&elf_header, 0,
              sizeof(elf_header)) != sizeof(elf_header))
        goto exec_failed;

    if (elf_header.magic != ELF_MAGIC)
        goto exec_failed;

    if ((pagetable = proc_pagetable(process)) == 0)
        goto exec_failed;

    if (load_elf_segments(pagetable, executable_inode, &elf_header,
                          &memory_size) < 0)
        goto exec_failed;

    iunlockput(executable_inode);
    end_op();
    executable_inode = 0;

    uint64 old_memory_size = process->memory_size;

    int argc = setup_user_stack(pagetable, &memory_size, argv, &stack);

    if (argc < 0)
        goto exec_failed;

    // "/bin/echo" → "echo"
    program_name = path;

    for (path_character = path; *path_character != '\0'; path_character++) {
        if (*path_character == '/')
            program_name = path_character + 1;
    }

    /*
     * 新しいuser imageが完成した。
     * ここから先で初めて現在のprocessを切り替える。
     */
    old_pagetable = process->pagetable;

    process->pagetable = pagetable;
    process->memory_size = memory_size;

    // argcは戻り値としてa0へ、argvはa1へ渡す。
    process->trapframe->epc = elf_header.entry;
    process->trapframe->sp = stack.pointer;
    process->trapframe->a1 = stack.argv_address;
    safestrcpy(process->name, program_name, sizeof(process->name));

    proc_freepagetable(old_pagetable, old_memory_size);

    return argc;

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
                        struct inode *inode, uint file_offset, uint byte_count)
{
    uint bytes_loaded, chunk_byte_count;
    uint64 physical_address;

    for (bytes_loaded = 0; bytes_loaded < byte_count; bytes_loaded += PGSIZE) {
        physical_address = walkaddr(pagetable, virtual_address + bytes_loaded);
        if (physical_address == 0)
            panic("loadseg: address should exist");
        if (byte_count - bytes_loaded < PGSIZE)
            chunk_byte_count = byte_count - bytes_loaded;
        else
            chunk_byte_count = PGSIZE;
        if (readi(inode, 0, physical_address, file_offset + bytes_loaded,
                  chunk_byte_count) != chunk_byte_count)
            return -1;
    }

    return 0;
}
