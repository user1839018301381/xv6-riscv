#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// 現在のプロセスのaddressからuint64を読み出す。
int fetchaddr(uint64 address, uint64 *value_out)
{
    struct proc *process = myproc();
    if (address >= process->memory_size ||
        address + sizeof(uint64) > process->memory_size) // オーバーフローに備えて両方の検査が必要
        return -1;
    if (copyin(process->pagetable, process->memory_size, (char *)value_out,
               address, sizeof(*value_out)) != 0)
        return -1;
    return 0;
}

// 現在のプロセスのaddressからNUL終端文字列を読み出す。
// 文字列長（NULを含まない）、またはエラー時に-1を返す。
int fetchstr(uint64 address, char *buffer, int max_length)
{
    struct proc *process = myproc();
    if (copyinstr(process->pagetable, process->memory_size, buffer,
                  address, max_length) < 0)
        return -1;
    return strlen(buffer);
}

static uint64 argraw(int argument_index)
{
    struct proc *process = myproc();
    switch (argument_index) {
    case 0:
        return process->trapframe->a0;
    case 1:
        return process->trapframe->a1;
    case 2:
        return process->trapframe->a2;
    case 3:
        return process->trapframe->a3;
    case 4:
        return process->trapframe->a4;
    case 5:
        return process->trapframe->a5;
    }
    panic("argraw");
    return -1;
}

// argument_index番目の32ビットシステムコール引数を読み出す。
void argint(int argument_index, int *value_out)
{
    *value_out = argraw(argument_index);
}

// 引数をポインタとして取得する。
// copyin/copyoutが検査するため、正当性は確認しない。
void argaddr(int argument_index, uint64 *address_out)
{
    *address_out = argraw(argument_index);
}

// argument_index番目のワードサイズのシステムコール引数を
// NUL終端文字列として読み出す。
// 最大max_lengthバイトをbufferへ複写する。
// 成功時は文字列長（NULを含まない）、エラー時は-1を返す。
int argstr(int argument_index, char *buffer, int max_length)
{
    uint64 address;
    argaddr(argument_index, &address);
    return fetchstr(address, buffer, max_length);
}

// システムコールを処理する関数のプロトタイプ。
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_sync(void);

// syscall.hのシステムコール番号から、
// そのシステムコールを処理する関数への対応配列。
static uint64 (*syscalls[])(void) = {
    // clang-format off
  [SYS_fork]    = sys_fork,
  [SYS_exit]    = sys_exit,
  [SYS_wait]    = sys_wait,
  [SYS_pipe]    = sys_pipe,
  [SYS_read]    = sys_read,
  [SYS_kill]    = sys_kill,
  [SYS_exec]    = sys_exec,
  [SYS_fstat]   = sys_fstat,
  [SYS_chdir]   = sys_chdir,
  [SYS_dup]     = sys_dup,
  [SYS_getpid]  = sys_getpid,
  [SYS_sbrk]    = sys_sbrk,
  [SYS_pause]   = sys_pause,
  [SYS_uptime]  = sys_uptime,
  [SYS_open]    = sys_open,
  [SYS_write]   = sys_write,
  [SYS_mknod]   = sys_mknod,
  [SYS_unlink]  = sys_unlink,
  [SYS_link]    = sys_link,
  [SYS_mkdir]   = sys_mkdir,
  [SYS_close]   = sys_close,
  [SYS_sync]    = sys_sync,
    // clang-format on
};

void syscall(void)
{
    int syscall_number;
    struct proc *process = myproc();

    syscall_number = process->trapframe->a7;
    if (syscall_number > 0 && syscall_number < NELEM(syscalls) &&
        syscalls[syscall_number]) {
        // syscall_numberに対応するシステムコール処理関数を呼び出し、
        // 戻り値をprocess->trapframe->a0に保存する。
        process->trapframe->a0 = syscalls[syscall_number]();
    } else {
        printk("%d %s: unknown sys call %d\n", process->pid,
               process->name, syscall_number);
        process->trapframe->a0 = -1;
    }
}
