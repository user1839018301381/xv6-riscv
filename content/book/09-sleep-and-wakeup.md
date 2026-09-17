---
title: "Sleep and Wakeup"
chapter: 9
source: "xv6-riscv-book"
sourceFile: "sleep.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
Scheduling and locks help conceal the actions of one thread from another, but we also need abstractions that help threads intentionally interact. For example, the reader of a pipe in xv6 may need to wait for a writing process to produce data; a parent's call to `wait` may need to wait for a child to exit; and a process reading the disk needs to wait for the disk hardware to finish the read. The xv6 kernel uses a mechanism called sleep and wakeup in these situations (and many others). Sleep allows a kernel thread to wait for some *condition* to be true; another thread or an interrupt handler can cause the condition to be true (typically by modifying some variable(s)) and then call wakeup to indicate that threads waiting for the condition should resume. Sleep and wakeup are often called *sequence coordination* or *conditional synchronization* mechanisms.

Before proceeding, please read the functions `sleep_prepare()`, `sleep()`, and `wakeup()` in [`kernel/proc.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c), and all of file [`kernel/pipe.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c).

## Overview

The sleep/wakeup interface looks like:

    void sleep_prepare(void *chan)
    void sleep()
    void wakeup(void *chan)

A process sleeps in order to wait for a condition to become true, and the `chan` argument (the *wait channel*) names the desired condition. `sleep_prepare(chan)` declares that a process might soon sleep in order to wait for the indicated wait channel; it returns right away. `wakeup(chan)` is called when the condition named by `chan` has (or might have) become true; it wakes up all processes that have called `sleep_prepare(chan)` and then `sleep()` with the same `chan`. `sleep()` returns right away if `wakeup(chan)` has already been called after the (required) previous call to `sleep_prepare(chan)`; if not, `sleep()` marks the calling process as `SLEEPING` (not `RUNNABLE`) and releases the CPU by context-switching to the scheduler, so that other processes can run. `sleep_prepare()` and `sleep()` are called by the same process (typically within a few lines of each other); `wakeup()` is called by a different process or by an interrupt handler.

These functions treat `chan` as an opaque 64-bit value; the only thing they do with it is compare for equality. The usual pattern is for callers to pass the address of some convenient object as the `chan` argument. The kernel programmer is responsible for choosing wait channel conventions, and for passing the same `chan` to `sleep_prepare()` and `wakeup()` calls that should interact.

Kernel code sleeps in order to wait for some condition to become true. For example, the kernel code that reads from a pipe sleeps if the pipe buffer is currently empty; the condition in this case is the pipe buffer becoming non-empty (due to another process writing to the pipe). `sleep_prepare()` and `sleep` do not know how to check the condition: only the calling code knows. The usual pattern is for the potentially sleeping process to first acquire a lock that protects the condition; then check the condition; and if it is not true, call `sleep_prepare()`, release the lock, and call `sleep()`. Code that later causes the condition to become true calls `wakeup`.

Here's a sketch of how the xv6 kernel pipe code sleeps:

    piperead(pipe){
      acquire(&pipe->lock);
      while(there's no data in pipe->buffer){
        sleep_prepare(&pipe);
        release(&pipe->lock);
        sleep();
        acquire(&pipe->lock);
      }
      remove the data from the pipe;
      release(&pipe->lock);
    }

    pipewrite(pipe){
      acquire(&pipe->lock);
      append data to pipe->buffer;
      wakeup(&pipe);
      release(&pipe->lock);
    }

This code uses the address of the pipe data structure as the wait channel.

`sleep_prepare(chan)` must be called either before checking the condition, or before releasing the lock that protects the condition. The call records the fact that the process may need to sleep on the indicated `chan`. If another process (or interrupt) calls `wakeup(chan)` between the calls to `sleep_prepare(chan)` and `sleep()`, `wakeup()` sets a flag in the about-to-sleep process indicating that it shouldn't actually sleep. If `sleep()` sees this flag, it returns rather than sleeping.

What is the reason for `sleep_prepare(chan)`? There is the possibility that the condition might become true, and `wakeup()` called, between when the pipe reader sees that the buffer is empty, and when it calls `sleep`. Without some cleverness, the `wakeup()` would not find any sleeping processes, and would not have any effect. Then the process would call `sleep()` even though the pipe buffer has data to read; and if (as could be the case) the pipe writer never wrote anything more, the reader could incorrectly sleep forever. This undesirable situation is called a *lost wake-up*. The behavior described in the previous paragraph avoids this problem.

In the pipe example, as in most sleeping code, there is a lock that protects the condition and prevents `wakeup()` from being called between when the going-to-sleep process checks the condition and when it calls `sleep_prepare()`. This lock is often called the *condition lock*. A process must release the lock before calling `sleep()` (so that some other process or interrupt handler can modify the condition and call `wakeup()`), and re-acquire the lock after `sleep()` returns.

## Code: Sleep and wakeup

Xv6's [`sleep_prepare`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L548) and [`sleep`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L563) and [`wakeup`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L577) implement the interface used in the example above. The basic idea is to have `sleep` mark the current process as `SLEEPING` and then call `sched` to release the CPU; `wakeup` looks for processes sleeping on the given wait channel and marks them `RUNNABLE`.

`sleep_prepare(chan)` records the wait channel in `p->chan` and returns. If `p->chan` is still non-zero when `sleep()` is called, `sleep()` changes the calling process to state `SLEEPING` and calls `sched()` to let other processes run.

`wakeup(chan)` handles two situations with respect to some other process that might concurrently be in the process of going to sleep on `chan`. If that other process has gotten as far as marking itself as `SLEEPING`, then `wakeup` will see it in that state and change it to `RUNNABLE`; its call to `sleep()` will then return. If the other process has gotten as far as calling `sleep_prepare(chan)` (but its `sleep()` has not gotten as far as acquiring `p->lock`), `wakeup(chan)` will see that `p->chan` is equal to `chan`, and `wakeup` will set `p->chan` to zero. In that case, when the other process does call `sleep`, it will see the zero and return immediately. The point is to avoid a lost wakeup if `wakeup()` is called just as another process is about to call `sleep()`.

The rules for use of `sleep_prepare()` prevent us from having to worry about the situation in which an about-to-sleep process hasn't yet called `sleep_prepare()`. Either that process surrounds its condition check and `sleep_prepare()` with a lock that prevents any call to `wakeup()` for the relevant channel, or that process must call `sleep_prepare()` before checking the condition.

Sometimes multiple processes are sleeping on the same channel; for example, more than one process reading from a pipe. A single call to `wakeup` will wake them all up. One of them will run first and (in the case of pipes) acquire the pipe lock and read whatever data is waiting. The other processes will find that, despite being woken up, there is no data to be read. From their point of view the wakeup was "spurious," and they must sleep again. For this reason sleeping always occurs inside a loop that re-checks the condition, as in `piperead` above.

No harm is done if two uses of sleep/wakeup accidentally choose the same channel: they will see spurious wakeups, but looping as described above will tolerate this problem. Much of the charm of sleep/wakeup is that it is both lightweight (no need to create special data structures to act as wait channels) and provides a layer of indirection (callers need not know which specific process they are interacting with).

## Code: Pipes

xv6's pipes are an example of code that sleeps to synchronize producers and consumers. We saw the interface for pipes in Chapter [1](/book/chapter-1/#CH:UNIX): bytes written to one end of a pipe are copied to an in-kernel buffer and then can be read from the other end of the pipe. Future chapters will examine the file descriptor support surrounding pipes, but let's look now at the implementations of `pipewrite` and `piperead`.

Each pipe is represented by a `struct pipe`, which contains a `lock` and a `data` buffer. The fields `nread` and `nwrite` count the total number of bytes read from and written to the buffer. The buffer wraps around: the next byte written after `buf[PIPESIZE-1]` is `buf[0]`. The counts do not wrap. This convention lets the implementation distinguish a full buffer (`nwrite` `==` `nread+PIPESIZE`) from an empty buffer (`nwrite` `==` `nread`), but it means that indexing into the buffer must use `buf[nread` `%` `PIPESIZE]` instead of just `buf[nread]` (and similarly for `nwrite`).

Let's suppose that calls to `piperead` and `pipewrite` happen simultaneously on two different CPUs. [`pipewrite`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L77) begins by acquiring the pipe's lock, which protects the counts, the data, and their associated invariants. [`piperead`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L112) then tries to acquire the lock too, but cannot. It spins in [`acquire`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L22) waiting for the lock. While `piperead` waits, `pipewrite` loops over the bytes being written (`addr[0..n-1]`), adding each to the pipe in turn [(pipe.c:101)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L101). During this loop, it could happen that the buffer fills [(pipe.c:88)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L88). In this case, `pipewrite` calls `wakeup` to alert any sleeping readers to the fact that there is data waiting in the buffer, releases the lock, and sleeps on channel `&pi->nwrite` to wait for a reader to take some bytes out of the buffer.

`piperead` now acquires the pipe's lock and enters its critical section: it finds that `pi->nread` `!=` [`pi->nwrite`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L119) (`pipewrite` went to sleep because `pi->nwrite` `==` `pi->nread` `+` [`PIPESIZE`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L88)), so it falls through to the `for` loop, copies data out of the pipe [(pipe.c:129)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L129), and increments `nread` by the number of bytes copied. That much space in the buffer is now available for writing, so `piperead` calls [`wakeup`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L140) to wake any sleeping writers before it returns. `wakeup` finds a process sleeping on `&pi->nwrite` (the process that was running `pipewrite` but stopped when the buffer filled). `wakeup` marks that process as `RUNNABLE`.

The pipe code uses separate wait channels for reader and writer (`&pi->nread` and `&pi->nwrite`); this might make the system more efficient in the event that there are lots of readers and writers waiting for the same pipe. The pipe code sleeps inside a loop checking the sleep condition; if there are multiple readers or writers, all but the first process to wake up will see the condition is false and sleep again.

## Code: Wait, exit, and kill

Please read the code for functions `kwait()`, `kexit()`, and `kkill()` in [`kernel/proc.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c); these are the internal implementations of the corresponding system calls.

An interesting example of sleeping, introduced in Chapter [1](/book/chapter-1/#CH:UNIX), is the interaction between a child's `exit` and its parent's `wait`. At the time of the child's death, the parent may already be sleeping in `wait`, or may be doing something else; in the latter case, a subsequent call to `wait` must observe the child's death, perhaps long after it calls `exit`. The way that xv6 records the child's demise until `wait` observes it is for `exit` to put the caller into the `ZOMBIE` state, where it stays until the parent's `wait` notices it, changes the child's state to `UNUSED`, copies the child's exit status, and returns the child's process ID to the parent. If the parent exits before the child, the parent gives the child to the `init` process, which perpetually calls `wait`; thus every child has a parent to clean up after it. A challenge is to avoid races and deadlock between simultaneous parent `wait` and child `exit`, as well as simultaneous `exit` and `exit`.

`kwait`, the kernel implementation for `wait`, starts by acquiring [`wait_lock`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L370), which acts as the condition lock that helps ensure that `kwait` doesn't miss a `wakeup` from an exiting child. Then `kwait` scans the process table. If it finds a child in `ZOMBIE` state, it frees that child's resources and its `proc` structure, copies the child's exit status to the address supplied to `wait` (if it is not 0), and returns the child's process ID. If `kwait` finds children but none have exited, it sleeps to wait for any of them to exit [(proc.c:414)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L414), then scans again. `kwait` often holds two locks, `wait_lock` and some process's `pp->lock`; the deadlock-avoiding order is first `wait_lock` and then `pp->lock`.

[`kexit`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L326) records the exit status, frees some resources, calls `reparent` to give any children to the `init` process, wakes up the parent in case it is in `wait`, marks the caller as a zombie, and permanently yields the CPU. `kexit` holds both `wait_lock` and `p->lock` during this sequence. It holds `wait_lock` because it's the condition lock for the `wakeup(p->parent)`, preventing a parent in `wait` from losing the wakeup. `kexit` must hold `p->lock` for this sequence also, to prevent a parent in `wait` from seeing that the child is in state `ZOMBIE` before the child has finally called `swtch`. `kexit` acquires these locks in the same order as `kwait` to avoid deadlock.

It may look incorrect for `kexit` to wake up the parent before setting its state to `ZOMBIE`, but that is safe: although `wakeup` may cause the parent to run, the loop in the parent's `kwait` cannot examine the child until the child's `p->lock` is released by `scheduler`, so `kwait` can't look at the exiting process until after `kexit` has set its state to [`ZOMBIE`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L358).

While `exit` allows a process to terminate itself, the `kill` system call [(proc.c:602)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L602) lets one process request that another terminate. It would be too complex for `kill` to directly destroy the victim process, since the victim might be executing on another CPU, perhaps in the middle of a sensitive sequence of updates to kernel data structures. Thus `kkill` does very little: it just sets the victim's `p->killed` and, if it is sleeping, wakes it up. Eventually the victim will enter or leave the kernel, at which point code in `usertrap` will call `kexit` if `p->killed` is set (it checks by calling [`killed`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L631)). If the victim is running in user space, it will see that it has been killed the next time it enters the kernel by making a system call or because the timer (or some other device) interrupts.

If the victim process is sleeping, `kkill` sets its state to `RUNNABLE` so that the victim returns from `sleep`. This is potentially dangerous because the condition being waited for may not be true. However, xv6 calls to `sleep` are always wrapped in a `while` loop that re-tests the condition after `sleep` returns. Some calls to `sleep` also test `p->killed` in the loop, and abandon the current activity if it is set. This is only done when such abandonment would be correct. For example, the pipe read and write code [(pipe.c:84)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c#L84) returns if the killed flag is set; eventually the code will return back to trap, which will again check `p->killed` and exit.

Some xv6 `sleep` loops do not check `p->killed` because the code is in the middle of a multi-step system call that should be atomic (i.e., would be incorrect if abandoned midway through). The virtio driver [(virtio_disk.c:287)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/virtio_disk.c#L287) is an example: it does not check `p->killed` because a disk operation may be one of a set of writes that are all needed in order for the file system to be left in a correct state. A process that is killed while waiting for disk I/O won't exit until it completes the current system call and `usertrap` sees the killed flag.

## Process Locking

The lock associated with each process (`p->lock`) is the most complex lock in xv6. A simple way to think about `p->lock` is that it must be held while reading or writing any of the following `struct proc` fields: `p->state`, `p->chan`, `p->killed`, `p->xstate`, and `p->pid`. These fields can be used by other processes, or by scheduler threads on other CPUs, so it's natural that they must be protected by a lock.

However, most uses of `p->lock` are protecting higher-level invariants of xv6's process data structures and algorithms. Here's the full set of things that `p->lock` does:

- Along with `p->state`, it prevents races in allocating `proc[]` slots for new processes.

- It conceals a process from view while it is being created or destroyed.

- It prevents a parent's `wait` from collecting a process that has set its state to `ZOMBIE` but has not yet yielded the CPU.

- It prevents another CPU's scheduler from deciding to run a yielding process after it sets its state to `RUNNABLE` but before it finishes `swtch`.

- It ensures that only one CPU's scheduler decides to run a `RUNNABLE` processes.

- It prevents a timer interrupt from causing a process to yield while it is in `swtch`.

- It prevents races between `sleep` and `wakeup`.

- It prevents the victim process of `kill` from exiting and perhaps being re-allocated between `kkill`'s check of `p->pid` and setting `p->killed`.

- It makes `kkill`'s check and write of `p->state` atomic.

The `p->parent` field is protected by the global lock `wait_lock` rather than by `p->lock`. Only a process's parent modifies `p->parent`, though the field is read both by the process itself and by other processes searching for their children. The purpose of `wait_lock` is to act as the condition lock when `wait` sleeps waiting for any child to exit. An exiting child holds either `wait_lock` or `p->lock` until after it has set its state to `ZOMBIE`, woken up its parent, and yielded the CPU. `wait_lock` also serializes concurrent `exit`s by a parent and child, so that the `init` process (which inherits the child) is guaranteed to be woken up from its `wait`. `wait_lock` is a global lock rather than a per-process lock in each parent, because, until a process acquires it, it cannot know who its parent is.

## Real world

`sleep` and `wakeup` are a simple and effective synchronization method, but there are many others; semaphores (Dijkstra 1965) are an example. The first challenge in all of them is to avoid the "lost wakeups" problem we saw at the beginning of the chapter. The original Unix kernel's `sleep` simply disabled interrupts, which sufficed because Unix ran on a single-CPU system. Plan 9's `sleep` uses a callback function that runs with the scheduling lock held just before going to sleep; the function serves as a last-minute check of the sleep condition, to avoid lost wakeups. The Linux kernel's `sleep` uses an explicit process queue, called a wait queue, instead of a wait channel; the queue has its own internal lock.

Scanning the entire set of processes in `wakeup` is inefficient. A better solution is to replace the `chan` in both `sleep` and `wakeup` with a data structure that holds a list of processes sleeping on that structure, such as Linux's wait queue. Plan 9's `sleep` and `wakeup` call that structure a rendezvous point. Many thread libraries refer to the same structure as a condition variable; in that context, the operations `sleep` and `wakeup` are called `wait` and `signal`. All of these mechanisms share the same flavor: the sleep condition is protected by some kind of lock dropped atomically during sleep.

xv6's `wakeup` wakes up all processes that are waiting on a particular wait channel. If there are more than one of them, they will all try to acquire the condition lock and re-check the condition; in many cases only one will be able to do anything useful (e.g., read all the data waiting in a pipe). The rest will find the condition is no longer true and go back to sleep; it was a waste of CPU time to wake them up. As a result, most condition variable designs provide two primitives: `signal`, which wakes up one of the processes waiting for the condition variable, and `broadcast`, which wakes up all of them.

Forcibly killing processes poses some problems. For example, a killed process may be deep inside the kernel sleeping, and unwinding its stack requires care, since each function on the call stack may need to do some clean-up. Some languages help out by providing an exception mechanism, but not C. Furthermore, there are other events that can cause a sleeping process to be woken up, even though the event it is waiting for has not happened yet. For example, when a Unix process is sleeping, another process may send a `signal` to it. In this case, the process will return from the interrupted system call with the value -1 and with the error code set to EINTR. The application can check for these values and decide what to do. Xv6 doesn't support signals and this complexity doesn't arise.

Xv6's support for `kill` is not entirely satisfactory: there are sleep loops which probably should check for `p->killed`. A related problem is that, even for `sleep` loops that check `p->killed`, there is a race between `sleep` and `kill`; the latter may set `p->killed` and try to wake up the victim just after the victim's loop checks `p->killed` but before it calls `sleep`. If this problem occurs, the victim won't notice the `p->killed` until the condition it is waiting for occurs. This may be quite a bit later or even never (e.g., if the victim is waiting for input from the console, but the user doesn't type any input).

## Exercises

1.  Implement counting semaphores in xv6. Choose a few of xv6's uses of sleep and wakeup and replace them with semaphores. Judge the result.

2.  Can you implement a sleep/wakeup scheme that just needs a call to `sleep()` (perhaps with additional arguments), and doesn't need anything like the call to `sleep_prepare()`?

3.  Fix the race mentioned above between `kill` and `sleep`, so that a `kill` that occurs after the victim's sleep loop checks `p->killed` but before it calls `sleep` results in the victim abandoning the current system call.

4.  Design a plan so that every sleep loop checks `p->killed` so that, for example, a process that is in the virtio driver can return quickly from the while loop if it is killed by another process.

## References
<span id="ref-dijkstra65"></span>
Dijkstra, Edsger. 1965. *Cooperating Sequential Processes*. [Https://www.cs.utexas.edu/users/EWD/transcriptions/EWD01xx/EWD123.html](https://www.cs.utexas.edu/users/EWD/transcriptions/EWD01xx/EWD123.html).
