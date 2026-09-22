#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64 sys_exit(void)
{
    int exit_status;
    argint(0, &exit_status);
    kexit(exit_status);
    return 0; // ここには到達しない
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return kfork(); }

uint64 sys_wait(void)
{
    uint64 status_address;
    argaddr(0, &status_address);
    return kwait(status_address);
}

uint64 sys_sbrk(void)
{
    uint64 previous_size;
    int allocation_mode;
    int size_delta;

    argint(0, &size_delta);
    argint(1, &allocation_mode);
    previous_size = myproc()->memory_size;

    if (allocation_mode == SBRK_EAGER || size_delta < 0) {
        if (growproc(size_delta) < 0) {
            return -1;
        }
    } else {
        // このプロセスのメモリを遅延割り当てする。
        // メモリサイズだけ増やし、メモリ自体は割り当てない。
        // プロセスがそのメモリを使うとvmfault()が割り当てる。
        if (previous_size + size_delta < previous_size)
            return -1;
        if (previous_size + size_delta > TRAPFRAME)
            return -1;
        myproc()->memory_size += size_delta;
    }
    return previous_size;
}

uint64 sys_pause(void)
{
    int tick_count;
    uint start_ticks;

    argint(0, &tick_count);
    if (tick_count < 0)
        tick_count = 0;
    acquire(&tickslock);
    start_ticks = ticks;
    while (ticks - start_ticks < tick_count) {
        if (is_killed(myproc())) {
            release(&tickslock);
            return -1;
        }
        sleep_prepare(&ticks);
        release(&tickslock);
        sleep();
        acquire(&tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64 sys_kill(void)
{
    int pid;

    argint(0, &pid);
    return kkill(pid);
}

// 起動後に発生したクロック割り込みの回数を返す。
uint64 sys_uptime(void)
{
    uint uptime_ticks;

    acquire(&tickslock);
    uptime_ticks = ticks;
    release(&tickslock);
    return uptime_ticks;
}
