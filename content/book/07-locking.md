---
title: "Locking"
chapter: 7
source: "xv6-riscv-book"
sourceFile: "lock.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
Most kernels, including xv6, interleave the execution of multiple activities. One source of interleaving is multiprocessor hardware: computers with multiple CPUs executing independently, such as xv6's RISC-V. These multiple CPUs share physical RAM, and xv6 exploits the sharing to maintain kernel data structures that all CPUs read and write. This sharing raises the possibility of one CPU reading a data structure while another CPU is mid-way through updating it, or even multiple CPUs updating the same data simultaneously; without careful design such parallel access is likely to yield incorrect results or a broken data structure. Even on a uniprocessor, the kernel may switch the CPU among a number of threads, causing their execution to be interleaved. Finally, a device interrupt handler that modifies the same data as some interruptible code could damage the data if the interrupt occurs just as the interrupted code is also modifying the data. The word *concurrency* refers to situations in which multiple instruction streams are interleaved, due to multiprocessor parallelism, thread switching, or interrupts.

Kernels are full of concurrently-accessed data. For example, two CPUs could simultaneously call `kalloc`, thereby concurrently popping from the head of the free list. Kernel designers like to allow for lots of concurrency, since it can increase performance and responsiveness. However, they must then convince themselves of correctness despite concurrency. Strategies aimed at correctness under concurrency, and abstractions that support them, are called *concurrency control* techniques.

This chapter focuses on a widely used concurrency control technique: the *lock*. As a preview, here's what use of a lock in the xv6 kernel looks like:

    struct spinlock lk;
    ...;
    acquire(&lk);
    ... read or write some shared data ...
    release(&lk);

A lock provides mutual exclusion, ensuring that only one CPU at a time can hold the lock. If the programmer associates a lock with each shared data item, and the code always holds the associated lock when using an item, then the item will be used by only one CPU at a time. In this situation, we say that the lock protects the data item. Although a lock is an easy-to-understand concurrency control mechanism, it can limit performance by serializing concurrent operations on the data it protects.

The rest of this chapter explains why xv6 needs locks, how xv6 implements them, and how it uses them.

## Races

<figure id="fig:smp" data-latex-placement="t">
<img src="/book/fig/smp.svg" alt="" loading="lazy">
<figcaption>
Figure 7.1: Simplified SMP architecture
</figcaption>
</figure>

As an example of why we need locks, consider two processes with exited children calling the `wait` system call on two different CPUs. `wait` frees the child's memory. Thus on each CPU, the kernel will call `kfree` to free the children's memory pages. The kernel allocator maintains a linked list of free pages: [`kalloc()`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c#L69) pops a page of memory from the list, and [`kfree()`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c#L47) pushes a page onto the list. For best performance, we might hope that the `kfree`s of the two parent processes would execute in parallel without either having to wait for the other, but this would not be correct given xv6's `kfree` implementation.

Figure [7.1](#fig:smp) illustrates the setting in more detail: the linked list of free pages is in memory that is shared by the two CPUs, which manipulate the list using load and store instructions. (In reality, the processors have caches, but conceptually multiprocessor systems behave as if there were a single, shared memory.) If there were no concurrent requests, you might implement a list `push` operation as follows:

```c
    struct element {
      int data;
      struct element *next;
    };

    struct element *list = 0;

    void
    push(int data)
    {
      struct element *l;

      l = malloc(sizeof *l);
      l->data = data;
      l->next = list;
      list = l;
   }
```

<figure id="fig:race" data-latex-placement="t">
<img src="/book/fig/race.svg" alt="" loading="lazy">
<figcaption>
Figure 7.2: Example race
</figcaption>
</figure>

This implementation is correct if executed in isolation. However, the code is not correct if more than one copy executes concurrently. If two CPUs execute `push` at the same time, both might execute line 15 as shown in Figure [7.1](#fig:smp), before either executes line 16, which results in an incorrect outcome as illustrated by Figure [7.2](#fig:race). There would then be two list elements with `next` set to the same former value of `list`. When the two assignments to `list` happen at line  16, the second one will overwrite the first; the element involved in the first assignment will be lost.

The lost update at line 16 is an example of a *race*. A race is a situation in which a memory location is accessed concurrently, and at least one access is a write. A race is often a sign of a bug, either a lost update (if the accesses are writes) or a read of an incompletely-updated data structure. The outcome of a race depends on the machine code generated by the compiler, the timing of the two CPUs involved, and how their memory operations are ordered by the memory system, which can make race-induced errors difficult to reproduce and debug. For example, adding print statements while debugging `push` might change the timing of the execution enough to make the race disappear.

The usual way to avoid races is to use a lock. Locks ensure *mutual exclusion*, so that only one CPU at a time can execute the sensitive lines of `push`; this makes the scenario above impossible. The correctly locked version of the above code adds just a few lines (highlighted in yellow):

```c
   struct element *list = 0;
   struct lock listlock;

   void
   push(int data)
   {
     struct element *l;
     l = malloc(sizeof *l);
     l->data = data;

     acquire(&listlock);
     l->next = list;
     list = l;
     release(&listlock);
   }
```

The sequence of instructions between `acquire` and `release` is often called a *critical section*. The lock is said to be protecting `list`.

When we say that a lock protects data, we really mean that the lock protects some collection of invariants that apply to the data. Invariants are properties of data structures that are maintained across operations. Typically, an operation's correct behavior depends on the invariants being true when the operation begins. The operation may temporarily violate the invariants but must reestablish them before finishing. For example, in the linked list case, the invariant is that `list` points at the first element in the list and that each element's `next` field points at the next element. The implementation of `push` violates this invariant temporarily: in line 17, `l` points to the next list element, but `list` does not point at `l` yet (reestablished at line 18). The race we examined above happened because a second CPU executed code that depended on the list invariants while they were (temporarily) violated. Proper use of a lock ensures that only one CPU at a time can operate on the data structure in the critical section, so that no CPU will execute a data structure operation when the data structure's invariants do not hold.

You can think of a lock as *serializing* concurrent critical sections so that they run one at a time, and thus preserve invariants (assuming the critical sections are correct in isolation). You can also think of critical sections guarded by the same lock as being atomic with respect to each other, so that each sees only the complete set of changes from earlier critical sections, and never sees partially-completed updates.

Though useful for correctness, locks inherently limit performance. For example, if two processes call `kfree` concurrently, the locks will serialize the two critical sections, so that there is no benefit from running them on different CPUs. We say that multiple processes *conflict* if they want the same lock at the same time, or that the lock experiences *contention*. A major challenge in kernel design is avoidance of lock contention in pursuit of parallelism. Xv6 does little of that, but sophisticated kernels organize data structures and algorithms specifically to avoid lock contention. In the list example, a kernel may maintain a separate free list per CPU and only touch another CPU's free list if the current CPU's list is empty and it must steal memory from another CPU. Other use cases may require more complicated designs.

The placement of locks is also important for performance. For example, it would be correct to move `acquire` earlier in `push`, before line 13. But this would likely reduce performance because then the calls to `malloc` would be serialized. The section "Using locks" below provides some guidelines for where to insert `acquire` and `release` invocations.

## Code: Locks

Please read [`kernel/spinlock.h`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.h) and [`kernel/spinlock.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c).

Xv6 has two types of locks: spinlocks and sleep-locks. We'll start with spinlocks. Xv6 represents a spinlock as a [`struct spinlock`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.h#L2). The important field in the structure is `locked`, a word that is zero when the lock is available and non-zero when it is held. Were there no concurrency, xv6 could acquire a lock with code like

```c
   void
   acquire(struct spinlock *lk) // does not work!
   {
     for(;;) {
       if(lk->locked == 0) {
         lk->locked = 1;
         break;
       }
     }
   }
```

Unfortunately, this implementation does not guarantee mutual exclusion in the face of concurrency. Two CPUs could simultaneously reach line 25, see that `lk->locked` is zero, and then both grab the lock by executing line 26. At this point, two different CPUs hold the lock, which violates the mutual exclusion property. What we need is a way to make lines 25 and 26 execute as an *atomic* (indivisible) step.

Because locks are widely used, multi-core processors usually provide an instruction that can be used to make lines 25 and 26 atomic. On the RISC-V this instruction is `amoswap r, r, a`. The `amoswap` instruction reads the value at memory address `a`, writes the contents of register `r` to that address, and puts the value it read into `r`. That is, it swaps the contents of the register and the addressed memory location. It performs this sequence atomically, using special hardware to prevent any other CPU from using the memory address between the read and the write.

The C compiler provides a family of "atomic" built-in functions that generate atomic instructions. Xv6's `acquire()` uses `__atomic_exchange_n(addr, value, ...)`, which boils down to the `amoswap` instruction; the function returns the old (swapped) contents of `addr`. Here's a good way to write the loop in `acquire`:

      while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0)
        ;

Xv6's [`acquire`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L22) uses the above loop (Section [7.6](/book/chapter-7/#SEC:MEMORDER) will explain `__ATOMIC_ACQUIRE`). Each iteration swaps one into `lk->locked` and checks the previous value; if the previous value is zero, then we've acquired the lock, and the swap will have set `lk->locked` to one. If the previous value is one, then some other CPU holds the lock, and the fact that we atomically swapped one into `lk->locked` didn't change its value.

Once the lock is acquired, `acquire` records, for debugging, the CPU that acquired the lock. The `lk->cpu` field is protected by the lock and must only be changed while holding the lock.

The function [`release`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L46) is the opposite of `acquire`: it clears the `lk->cpu` field and then releases the lock. Conceptually, the release just assigns zero to `lk->locked`. The C standard allows compilers to implement an assignment with multiple store instructions, so a C assignment might be non-atomic with respect to concurrent code. Instead, `release` uses the C atomic function `__atomic_store_n(addr, 0, ...)` that stores zero to the address in a single indivisible operation.

## Code: Using locks

Xv6 uses locks in many places to avoid races. As described above, [`kalloc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c#L69) and [`kfree`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c#L47) form a good example. Try Exercises 1 and 2 to see what happens if those functions omit the locks. You'll likely find that it's difficult to trigger incorrect behavior, suggesting that it's hard to reliably test whether code is free from locking errors and races. Xv6 may well have as-yet-undiscovered races.

A hard part about using locks is deciding how many locks to use and which data and invariants each lock should protect. There are a few basic principles. First, in any code that might cause a memory location to be written by one CPU at the same time that another CPU reads or writes it, a lock should be used to keep the two operations from overlapping. Second, remember that locks protect invariants: if an invariant involves multiple memory locations, typically all of them need to be protected by a single lock to ensure the invariant is maintained.

The rules above say when locks are necessary but say nothing about when locks are unnecessary, and it is important for efficiency not to lock too much, because locks reduce parallelism. If parallelism isn't important, then one could arrange to have only a single thread and not worry about locks. A simple kernel can do this on a multiprocessor by having a single lock; the kernel acquires the lock every time the kernel is entered from user space, for a system call or interrupt; the kernel releases the lock when it returns to user space. Many uniprocessor operating systems have been converted to run on multiprocessors using this approach, sometimes called a "big kernel lock," but the approach sacrifices parallelism: only one CPU can execute in the kernel at a time. If the kernel consumes significant CPU time, more parallelism could be obtained by protecting different objects or modules with different locks, so that different CPUs could be executing in different parts of the kernel at the same time.

As an example of coarse-grained locking, xv6's `kalloc.c` allocator has a single free list protected by a single lock. If multiple processes on different CPUs try to allocate pages at the same time, each will have to wait for its turn by spinning in `acquire`. Spinning wastes CPU time, since it's not useful work. If contention for the lock wasted a significant fraction of CPU time, perhaps performance could be improved by changing the allocator to have a separate free list per CPU, each with its own lock, to allow truly parallel allocation.

As an example of fine-grained locking, xv6 has a separate lock for each file, so that processes that manipulate different files can often proceed without waiting for each other's locks. The file locking scheme could be made even more fine-grained if one wanted to allow processes to simultaneously write different areas of the same file. Ultimately lock granularity decisions need to be driven by performance requirements as well as complexity considerations.

As subsequent chapters explain each part of xv6, they will mention examples of xv6's use of locks to deal with concurrency. As a preview, Figure [7.3](#fig:locktable) lists all of the locks in xv6.

<figure id="fig:locktable" data-latex-placement="t">
<table>
<thead>
<tr>
<th style="text-align: left;">
<strong>Lock</strong>
</th>
<th style="text-align: left;">
<strong>Description</strong>
</th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;">
bcache.lock
</td>
<td style="text-align: left;">
Protects allocation of block buffer cache entries
</td>
</tr>
<tr>
<td style="text-align: left;">
cons.lock
</td>
<td style="text-align: left;">
Serializes read processing of console input
</td>
</tr>
<tr>
<td style="text-align: left;">
tx_lock
</td>
<td style="text-align: left;">
Serializes access to console (uart) output hardware
</td>
</tr>
<tr>
<td style="text-align: left;">
ftable.lock
</td>
<td style="text-align: left;">
Serializes allocation of a struct file in file table
</td>
</tr>
<tr>
<td style="text-align: left;">
itable.lock
</td>
<td style="text-align: left;">
Protects allocation of in-memory inode entries
</td>
</tr>
<tr>
<td style="text-align: left;">
vdisk_lock
</td>
<td style="text-align: left;">
Serializes access to disk hardware and queue of DMA descriptors
</td>
</tr>
<tr>
<td style="text-align: left;">
kmem.lock
</td>
<td style="text-align: left;">
Serializes allocation of memory
</td>
</tr>
<tr>
<td style="text-align: left;">
log.lock
</td>
<td style="text-align: left;">
Serializes operations on the transaction log
</td>
</tr>
<tr>
<td style="text-align: left;">
pipe's pi-\>lock
</td>
<td style="text-align: left;">
Serializes operations on each pipe
</td>
</tr>
<tr>
<td style="text-align: left;">
pid_lock
</td>
<td style="text-align: left;">
Serializes increments of next_pid
</td>
</tr>
<tr>
<td style="text-align: left;">
proc's p-\>lock
</td>
<td style="text-align: left;">
Serializes changes to each process's state
</td>
</tr>
<tr>
<td style="text-align: left;">
wait_lock
</td>
<td style="text-align: left;">
Helps wait avoid lost wakeups
</td>
</tr>
<tr>
<td style="text-align: left;">
tickslock
</td>
<td style="text-align: left;">
Serializes operations on the ticks counter
</td>
</tr>
<tr>
<td style="text-align: left;">
inode's ip-\>lock
</td>
<td style="text-align: left;">
Serializes operations on each inode and its content
</td>
</tr>
<tr>
<td style="text-align: left;">
buf's b-\>lock
</td>
<td style="text-align: left;">
Serializes operations on each block buffer
</td>
</tr>
</tbody>
</table>
<figcaption>
Figure 7.3: Locks in xv6
</figcaption>
</figure>

## Deadlock and lock ordering

Suppose the functions running on CPUs C1 and C2 both have a point at which each needs to hold both lock A and lock B, and they acquire them in different orders:

::: center
<figure><img src="/book/fig/order.svg" alt="image" loading="lazy"><figcaption>image</figcaption></figure>

With a bit of bad luck, C1 and C2 might both execute their first acquire at exactly the same moment; both can succeed, since they are asking for different locks. But then both C1 and C2 will have to wait in their second calls to `acquire()`, since both locks are already held by the other CPU. Because both CPUs are waiting for each other, neither will ever release a lock, and both will wait forever. This situation is called *deadlock*.

The key problem in the C1/C2 example is that the two CPUs acquired the locks in different orders. If they had both tried to acquire A first, one would have acquired A and then B and then released them both, and then the other CPU could have proceeded. More generally, locking code must follow this rule to avoid deadlock: all code paths that hold multiple locks must acquire locks in the same order. The need for this global lock acquisition order means that locks are effectively part of each function's specification: callers must invoke functions in a way that causes locks to be acquired in the agreed-on order.

Xv6 has many lock-order chains of length two involving per-process locks (the lock in each `struct proc`) due to the way that the `sleep`/`wakeup` mechanism works (see Chapter [9](/book/chapter-9/#CH:SLEEP)). For example, [`consoleintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c#L147) is the interrupt routine which handles typed characters. When a newline arrives, any process that is waiting for console input should be woken up. To do this, `consoleintr` holds `cons.lock` while calling `wakeup`, which acquires the waiting process's lock in order to wake it up. In consequence, the global deadlock-avoiding lock order includes the rule that `cons.lock` must be acquired before any process lock. The file-system code contains xv6's longest lock chains. For example, creating a file requires simultaneously holding a lock on the directory, a lock on the new file's inode, a lock on a disk block buffer, the disk driver's `vdisk_lock`, and the calling process's `p->lock`. To avoid deadlock, file-system code always acquires locks in the order mentioned in the previous sentence.

Honoring a global deadlock-avoiding order can be surprisingly difficult. Sometimes the lock order conflicts with logical program structure, e.g., perhaps code module M1 calls module M2, but the lock order requires that a lock in M2 be acquired before a lock in M1. Sometimes the identities of locks aren't known in advance, perhaps because one lock must be held in order to discover the identity of the lock to be acquired next. This kind of situation arises in the file system as it looks up successive components in a path name, and in the code for the `wait` and `exit` system calls as they search the table of processes looking for child processes. Finally, the danger of deadlock is often a constraint on how fine-grained one can make a locking scheme, since more locks often means more opportunity for deadlock. The need to avoid deadlock is often a major factor in kernel implementation.

A question that sometimes arises is what should happen if a CPU tries to acquire a lock that the same CPU already holds. One line of reasoning is that this should be allowed: no other CPU can hold the lock, so there's no need to worry about CPUs interfering with each others' use of the protected data. Locking systems that allow a CPU to re-acquire a lock it already holds are called a *re-entrant* or *recursive*. On the other hand, if a lock is already held, even on the same CPU, that means an operation may have temporarily violated and not yet restored some invariants; to allow a new operation to commence while the invariants don't hold seems like an invitation to bugs. Xv6 takes this latter view, and forbids a CPU that currently holds a lock from re-acquiring it. Detecting this situation is the purpose of the call to `holding` in `acquire`.

<a id="s:lockinter"></a>

## Locks and interrupts
Some xv6 spinlocks protect data that is used by both threads and interrupt handlers. For example, the `clockintr` timer interrupt handler might increment [`ticks`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L167) at about the same time that a kernel thread reads `ticks` in [`sys_pause`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysproc.c#L77). The lock `tickslock` serializes the two accesses.

The interaction of spinlocks and interrupts raises a potential danger. Suppose `sys_pause` holds `tickslock`, and its CPU is interrupted by a timer interrupt. `clockintr` would try to acquire `tickslock`, see it was held, and wait for it to be released. In this situation, `tickslock` will never be released: only `sys_pause` can release it, but `sys_pause` will not continue running until `clockintr` returns. So the CPU will deadlock, and any code that needs either lock will also freeze.

To avoid this situation, if a spinlock is used by an interrupt handler, a CPU must never hold that lock with interrupts enabled. Xv6 is more conservative: when a CPU acquires any lock, xv6 always disables interrupts on that CPU. Interrupts may still occur on other CPUs, so an interrupt's `acquire` can wait for a thread to release a spinlock; just not on the same CPU.

Xv6 re-enables interrupts when a CPU holds no spinlocks; it must do a little book-keeping to cope with nested critical sections. `acquire` calls [`push_off`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L93) and `release` calls [`pop_off`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L106) to track the nesting level of locks on the current CPU. When that count reaches zero, `pop_off` restores the interrupt enable state that existed at the start of the outermost critical section. `push_off` and `pop_off` tell the CPU to enable and disable interrupts by changing the `SIE` (Supervisor Interrupt Enable) bit in the `sstatus` (Supervisor Status) register, using `intr_on` and `rc_sstatus(SSTATUS_SIE)`; the latter clears the `SIE` bit.

It is important that `acquire` call `push_off` strictly before setting [`lk->locked`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L37). If the two were reversed, there would be a brief window when the lock was held with interrupts enabled, and an unfortunately timed interrupt would deadlock the system. Similarly, it is important that `release` call `pop_off` only after releasing the lock [(spinlock.c:73)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c#L73).

<a id="SEC:MEMORDER"></a>

## Instruction and memory ordering
It is natural to think of programs executing in the order in which source code statements appear. That's a reasonable mental model for single-threaded code, but is not what actually happens.

One reason is that compilers emit load and store instructions in orders different from those implied by the source code, and may entirely omit them (for example by caching data in registers). Another reason is that the CPU may execute instructions out of order to increase performance. For example, a CPU may notice that in a serial sequence of instructions A and B are not dependent on each other. The CPU may start instruction B first, either because its inputs are ready before A's inputs, or in order to overlap execution of A and B.

Memory access re-ordering by the CPU and compiler can cause problems when multiple threads interact through shared memory (Waterman and Asanovic 2024; Boehm 2005). As an example of what could go wrong, in this code for `push`, it would be a disaster if the compiler or CPU moved the store corresponding to line 4 to a point after the `release` on line 6:

```c
      l = malloc(sizeof *l);
      l->data = data;
      acquire(&listlock);
      l->next = list;
      list = l;
      release(&listlock);
```

If such a re-ordering occurred, there would be a window during which another CPU could acquire the lock and observe the updated `list`, but see an uninitialized `list->next`.

The good news is that compilers and CPUs provide ways for programmers to prevent re-ordering. These appear in xv6 as the `__ATOMIC_ACQUIRE` and `__ATOMIC_RELEASE` arguments to the C atomic functions used in `acquire` and `release`. Each has two effects. First, each tells the compiler not to move memory references past the atomic call. Second, each tells the compiler to generate instructions that tell the CPU not to move execution-time memory references past the instructions generated for the atomic call. Such a "don't move past" point is called a *memory barrier* or a *fence*.

The barriers in xv6's spinlock functions have two important effects on code that uses locks. First, all memory references (reads and writes) that occur in the C code before a `release()` are guaranteed to take effect on the memory system before the `release()` completes. This includes both memory references in the critical section and memory references before the critical section. Second, all memory references in the C code after the `acquire` are guaranteed **not** to take effect before the successful completion of the `amoswap` in `acquire()`. Again, this includes both references inside the critical section and references after the `release`.

These barriers produce an important transitive effect. For example, if a CPU writes to a newly-allocated data object (without holding locks) and then, in a critical section, links that object into a list, any CPU that subsequently acquires a lock and obtains a pointer to the object from the list is guaranteed to observe the first CPU's writes.

The barriers in xv6's `acquire` and `release` force order in almost all cases where it matters, since xv6 uses locks around accesses to shared data. Chapter [12](/book/chapter-12/#CH:LOCK2) discusses a few exceptions.

## Programming with Atomics

Though xv6 doesn't do this, it is sometimes possible to update shared data while avoiding races by direct use of atomic instructions, without use of locks. Most CPUs conveniently supply dozens of atomic instructions, typically with the pattern that they read a memory location, modify the value, and write the new value to the same memory location, atomically. The RISC-V `amoadd` instruction is an example: it atomically adds a register's contents to a location in memory. The C compiler provides "atomic" wrappers for these instructions.[^1] Thus code could increment a shared counter without locks using

      __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);

It can be useful to avoid locks because they consume CPU time, or because a structural reason such as deadlock avoidance makes use of locks awkward.

## Sleep locks

Sometimes xv6 needs to hold a lock for a long time. For example, the file system (Chapter [10](/book/chapter-10/#CH:FS)) keeps a file locked while reading and writing its content on the disk, and these disk operations can take tens of milliseconds. Holding a spinlock that long would lead to waste if another process wanted to acquire it, since the acquiring process would waste CPU for a long time while spinning. Another drawback of spinlocks is that a process cannot yield the CPU while retaining a spinlock; we'd like to do this so that other processes can use the CPU while the process with the lock waits for the disk. Yielding while holding a spinlock is illegal because it might lead to deadlock if a second thread then tried to acquire the spinlock; since `acquire` doesn't yield the CPU, the second thread's spinning might prevent the first thread from running and releasing the lock. Yielding while holding a lock would also violate the requirement that interrupts must be off while a spinlock is held. Thus we'd like a type of lock that yields the CPU while waiting to acquire, and allows yields (and interrupts) while the lock is held.

Xv6 provides such locks in the form of *sleep-locks*. [`acquiresleep`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c#L22) yields the CPU while waiting, using techniques that will be explained in Chapter [9](/book/chapter-9/#CH:SLEEP). At a high level, a sleep-lock has a `locked` field that is protected by a spinlock. If the sleep-lock is held, `acquiresleep` releases the spinlock and calls `sleep`, which yields the CPU until `releasesleep` calls `wakeup`. The result is that other threads can execute while `acquiresleep` waits.

Because sleep-locks leave interrupts enabled, they cannot be used in interrupt handlers. Because `acquiresleep` may yield the CPU, sleep-locks cannot be used inside spinlock critical sections (though spinlocks can be used inside sleep-lock critical sections).

Spin-locks are best suited to short critical sections, since waiting for them wastes CPU time; sleep-locks work well for lengthy operations.

## Real world

Programming with locks remains challenging despite years of research into concurrency primitives and parallelism. It is often best to conceal locks within higher-level constructs like synchronized queues, although xv6 does not do this. If you program with locks, it is wise to use a tool that attempts to identify races, because it is easy to miss an invariant that requires a lock.

Most operating systems support POSIX threads (Pthreads), which allow a user process to have several threads running concurrently on different CPUs. Pthreads has support for user-level locks, barriers, etc. Pthreads also allows a programmer to optionally specify that a lock should be re-entrant.

Supporting Pthreads at user level requires support from the operating system. For example, it should be the case that if one pthread blocks in a system call, another pthread of the same process should be able to run on that CPU. As another example, if a pthread changes its process's address space (e.g., maps or unmaps memory), the kernel must arrange that other CPUs that run threads of the same process update their hardware page tables to reflect the change in the address space.

It is possible to implement locks without atomic instructions (Lamport 1974), but it is expensive, so most operating systems use atomic instructions.

Locks can be expensive if many CPUs try to acquire the same lock at the same time. If one CPU has a lock cached in its local cache, and another CPU must acquire the lock, then the atomic instruction to update the cache line that holds the lock must move the line from the one CPU's cache to the other CPU's cache, and perhaps invalidate any other copies of the cache line. Fetching a cache line from another CPU's cache can be orders of magnitude more expensive than fetching a line from a local cache.

To avoid the expenses associated with locks, many operating systems use lock-free data structures and algorithms (Herlihy and Shavit 2012; Mckenney et al. 2013). For example, it is possible to implement a linked list like the one in the beginning of the chapter that requires no locks during list searches, and one atomic instruction to insert an item in a list. Lock-free programming is more complicated, however, than programming locks; for example, one must worry about instruction and memory reordering. Programming with locks is already hard, so xv6 avoids the additional complexity of lock-free programming.

## Exercises

1.  Comment out the calls to `acquire` and `release` in [`kalloc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c#L69). This seems like it should cause problems for kernel code that calls `kalloc`; what symptoms do you expect to see? When you run xv6, do you see these symptoms? How about when running `usertests`? If you don't see a problem, why not? See if you can provoke a problem by inserting dummy loops into the critical section of `kalloc`.

2.  Suppose that you instead commented out the locking in `kfree` (after restoring locking in `kalloc`). What might now go wrong? Is lack of locks in `kfree` less harmful than in `kalloc`?

3.  If two CPUs call `kalloc` at the same time, one will have to wait for the other, which is bad for performance. Modify `kalloc.c` to have more parallelism, so that simultaneous calls to `kalloc` from different CPUs can proceed without waiting for each other.

4.  Write a parallel program using POSIX threads, which is supported on most operating systems. For example, implement a parallel hash table and measure if the number of puts/gets scales with increasing number of CPUs.

5.  Implement a subset of Pthreads in xv6. That is, implement a user-level thread library so that a user process can have more than 1 thread and arrange that these threads can run in parallel on different CPUs. Come up with a design that correctly handles a thread making a blocking system call and changing its shared address space.

## References
<span id="ref-boehm04"></span>
Boehm, Hans-J. 2005. "Threads Cannot Be Implemented as a Library." *ACM PLDI Conference*.

<span id="ref-herlihy:art"></span>
Herlihy, Maurice, and Nir Shavit. 2012. *The Art of Multiprocessor Programming, Revised Reprint*. Morgan Kaufmann.

<span id="ref-lamport:bakery"></span>
Lamport, L. 1974. "A New Solution of Dijkstra's Concurrent Programming Problem." *Communications of the ACM*.

<span id="ref-mckenney:rcuusage"></span>
Mckenney, Paul E., Silas Boyd-wickizer, and Jonathan Walpole. 2013. *RCU Usage in the Linux Kernel: One Decade Later*.

<span id="ref-riscv:user"></span>
Waterman, Andrew, and Krste Asanovic, eds. 2024. *The RISC-V Instruction Set Manual Volume I: Unprivileged Specification ISA*. [Https://drive.google.com/file/d/17GeetSnT5wW3xNuAHI95-SI1gPGd5sJ\_/view?usp=drive_link](https://drive.google.com/file/d/17GeetSnT5wW3xNuAHI95-SI1gPGd5sJ_/view?usp=drive_link).


[^1]: <https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html>
