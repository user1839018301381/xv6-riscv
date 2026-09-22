// スリープロック

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void initsleeplock(struct sleeplock *lock, char *name)
{
    initlock(&lock->spinlock, "sleep lock");
    lock->name = name;
    lock->is_locked = 0;
    lock->owner_pid = 0;
}

void acquiresleep(struct sleeplock *lock)
{
    acquire(&lock->spinlock);
    while (lock->is_locked) {
        sleep_prepare(lock);
        release(&lock->spinlock);
        sleep();
        acquire(&lock->spinlock);
    }
    lock->is_locked = 1;
    lock->owner_pid = myproc()->pid;
    release(&lock->spinlock);
}

void releasesleep(struct sleeplock *lock)
{
    acquire(&lock->spinlock);
    lock->is_locked = 0;
    lock->owner_pid = 0;
    wakeup(lock);
    release(&lock->spinlock);
}

int holdingsleep(struct sleeplock *lock)
{
    int is_held;

    acquire(&lock->spinlock);
    is_held = lock->is_locked && (lock->owner_pid == myproc()->pid);
    release(&lock->spinlock);
    return is_held;
}
