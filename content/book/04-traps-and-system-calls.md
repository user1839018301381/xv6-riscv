---
title: "Traps and system calls"
chapter: 4
source: "xv6-riscv-book"
sourceFile: "trap.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
There are three kinds of event which cause the CPU to set aside ordinary execution of instructions and force a transfer of control to special kernel code that handles the event. One situation is a system call, when a user program executes the `ecall` instruction to ask the kernel to do something for it. Another situation is an *exception*: an instruction (user or kernel) does something illegal, such as load from an invalid virtual address. The third situation is a device *interrupt*, when a device signals that it needs attention, for example when the disk hardware finishes a read or write request.

This book uses *trap* as a generic term for these situations. Typically whatever code was executing at the time of the trap will later need to resume, and shouldn't need to be aware that anything special happened. That is, we often want traps to be transparent; this is particularly important for device interrupts, which the interrupted code typically doesn't expect. A trap forces a transfer of control into the kernel; the kernel saves registers and other state so that execution can be resumed; the kernel executes appropriate handler code (e.g., a system call implementation or device driver); the kernel restores the saved state and returns from the trap; and the original code resumes where it left off.

Xv6 handles all traps in the kernel; traps are not delivered to user code. Handling traps in the kernel is natural for system calls. It makes sense for interrupts since isolation demands that only the kernel be allowed to use devices, and because the kernel is able to share devices among multiple processes. It also makes sense for exceptions since the kernel may be able to handle the exception from user space (for an example see Chapter [5](/book/chapter-5/#CH:PGFAULTS)) or respond by killing the offending program.

Xv6 trap handling proceeds in four stages: hardware actions taken by the RISC-V CPU, some assembly instructions that prepare the way for kernel C code, a C function that decides what to do with the trap, and the system call or device-driver service routine. While commonality among the three trap types suggests that a kernel could handle all traps with a single code path, it turns out to be convenient to have separate code for two distinct cases: traps from user space, and traps from kernel space. Kernel code (assembler or C) that processes a trap is often called a *handler*; the first handler instructions are usually written in assembler (rather than C) and are sometimes called a *vector*.

Before proceeding, please read [`kernel/trampoline.S`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trampoline.S), and `usertrap()` and `prepare_return()` in [`kernel/trap.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c).

## RISC-V trap machinery

Each RISC-V CPU has a set of hardware control registers that the kernel writes to tell the CPU how to handle traps, and that the kernel can read to find out about a trap that has occurred. The RISC-V documents contain the full story (Waterman et al. 2024). [`riscv.h`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/riscv.h) contains definitions that xv6 uses. Here's an outline of the most important registers:

- `stvec`: The kernel writes the address of its trap handler code here; the RISC-V jumps to the address in `stvec` to handle a trap.

- `sepc`: When a trap occurs, RISC-V saves the program counter here (since the `pc` is then overwritten with the value in `stvec`). The `sret` (return from trap) instruction copies `sepc` to the `pc`. The kernel can write `sepc` to control where `sret` goes.

- `scause`: RISC-V puts a number here that describes the reason for the trap.

- `sscratch`: The kernel trap handler code uses `sscratch` to help it avoid overwriting user registers before saving them.

- `sstatus`: The SIE bit in `sstatus` controls whether device interrupts are enabled. If the kernel clears SIE, the RISC-V will defer device interrupts until the kernel sets SIE. The SPP bit indicates whether a trap came from user mode or supervisor mode, and controls to what mode `sret` returns.

The above registers can only be accessed in supervisor mode (i.e., by the kernel); the CPU prevents user code from reading or writing them.

Each CPU on a multi-core chip has its own set of these registers, and more than one CPU may be handling a trap at any given time.

When it forces a trap, the RISC-V hardware does the following:

1.  If the trap is a device interrupt, and the `sstatus` SIE bit is clear, don't do any of the following.

2.  Disable interrupts by clearing the SIE bit in `sstatus`.

3.  Copy the `pc` to `sepc`.

4.  Save the current mode (user or supervisor) in the SPP bit in `sstatus`.

5.  Set `scause` to a number indicating the trap's cause.

6.  Set the mode to supervisor.

7.  Copy `stvec` to the `pc`.

8.  Start executing at the new `pc`.

The CPU doesn't switch to the kernel page table, doesn't switch to a stack in the kernel, and doesn't save any registers other than the `pc`. Kernel software must perform these tasks. One reason that the CPU does minimal work during a trap is to provide flexibility to software; for example, some operating systems omit a page table switch in some situations to increase trap performance.

It's worth thinking about whether any of the steps listed above could be omitted, perhaps in search of faster traps. Though there are situations in which a simpler sequence can work, many of the steps would be dangerous to omit in general. For example, suppose that the CPU didn't switch program counters. Then a trap from user space could switch to supervisor mode while still running user instructions. Those user instructions could break user/kernel isolation, for example by modifying the `satp` register to point to a page table that allowed accessing all of physical memory. It is thus important that the CPU switch to a kernel-specified instruction address, namely `stvec`.

## Traps from user space

<figure id="fig:usertrap" data-latex-placement="t">

::: center
<img src="/book/fig/trap.svg" alt="" loading="lazy">

<figcaption>
Figure 4.1: Outline of how a trap from user code is handled.
</figcaption>
</figure>

Xv6 handles traps differently depending on whether the trap occurs while executing in the kernel or in user code. Here is the story for traps from user code; Section [4.5](/book/chapter-4/#s:ktraps) describes traps from kernel code.

A trap may occur while executing in user space if the user program makes a system call (`ecall` instruction), or does something illegal, or if a device interrupts. As shown in Figure [4.1](#fig:usertrap), the high-level path of a trap from user space is [`uservec`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trampoline.S#L22), then [`usertrap`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L38); and when the kernel is ready to return, `usertrap` returns to [`userret`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trampoline.S#L101) which executes `sret` to user space.

A major constraint on the design of xv6's trap handling is the fact that the RISC-V hardware does not switch page tables when it forces a trap. This means that the trap handler address in `stvec` must have a valid mapping in the user page table, since that's the page table in force when the trap handling code starts executing. Furthermore, xv6's trap handling code needs to switch to the kernel page table; in order to be able to continue executing after that switch, the kernel page table must also have a mapping for the handler pointed to by `stvec`.

Xv6 satisfies these requirements using a *trampoline* page. This page contains `uservec`, the xv6 trap handling code that `stvec` points to. The trampoline page is mapped in every process's page table at virtual address `0x3ffffff000` (called `TRAMPOLINE`), which is the last page in the virtual address space so that it will be above memory that programs use for themselves. The trampoline page is mapped at the same virtual address in the kernel page table. See Figure [2.3](#fig:as) and Figure [3.3](#fig:xv6_layout). Because the trampoline page is mapped in the user page table, traps can start executing there in supervisor mode. Because the trampoline page is mapped at the same address in the kernel address space, the trap handler can continue to execute after it switches to the kernel page table.

The code for the `uservec` trap handler is in [`trampoline.S`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trampoline.S#L22). When `uservec` starts, all 32 registers contain values owned by the interrupted user code. These 32 values need to be saved somewhere in memory, so that later on the kernel can restore them before returning to user space. Storing to memory requires use of a register to hold the store's destination address, but at this point there are no general-purpose registers available! Luckily RISC-V provides a helping hand in the form of the `sscratch` register. The `csrw` instruction at the start of `uservec` saves `a0` in `sscratch`. Now `uservec` has one register (`a0`) to play with.

`uservec`'s next task is to save the 32 user registers. The kernel allocates, for each process, a page of memory for a `trapframe` structure that (among other things) has space to save the 32 user registers [(proc.h:40)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.h#L40). Because `satp` still refers to the user page table, `uservec` needs the trapframe to be mapped in the user address space. Xv6 maps each process's trapframe at virtual address `TRAPFRAME` (`0x3fffffe000`) in that process's user page table; one page below `TRAMPOLINE`. Each process's `p->trapframe` contains a kernel virtual address for the process's trapframe.

`uservec` sets register `a0` to address `TRAPFRAME` and saves all the user registers there. Then it retrieves the user `a0` from `sscratch` and saves it in the trapframe.

The kernel previously initialized the trapframe to contain some values useful to `uservec`: the address of the current process's kernel stack, the current CPU's hartid, the address of the `usertrap` function, and the address of the kernel page table. `uservec` retrieves these values, switches `satp` to the kernel page table, and jumps to `usertrap`, a C function.

The job of `usertrap` is to determine the cause of the trap, process it, and return [(trap.c:38)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L38). It first changes `stvec` so that a trap while in the kernel will be handled by `kernelvec` rather than `uservec`. It saves the `sepc` register (the saved user program counter) for future use when returning back to user space. If the trap is a system call, `usertrap` calls `syscall` to handle it; if a device interrupt, `devintr`; if a page fault, `vmfault`; otherwise it's an exception (e.g., use of an invalid address), and the kernel kills the faulting process. The system call path adds four to the saved user program counter because RISC-V, in the case of a system call, leaves the program pointer pointing to the `ecall` instruction but user code needs to resume executing at the subsequent instruction. `usertrap` checks if the process has been killed or should yield the CPU (if this trap is a timer interrupt).

The first step in returning to user space is the call to [`prepare_return`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L101). This function sets up the RISC-V control registers to prepare for a future trap from user space: setting `stvec` to `uservec` and preparing the trapframe fields that `uservec` relies on. `prepare_return` sets `sepc` to the previously saved user program counter. Finally, `usertrap` returns back to `userret` in the trampoline page [(trampoline.S:101)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trampoline.S#L101), passing back a pointer to the user page table in `a0`.

`userret` switches `satp` to the process's user page table. Recall that the user page table maps both the trampoline page and `TRAPFRAME`, but nothing else from the kernel. The trampoline page mapping at the same virtual address in user and kernel page tables allows `userret` to keep executing after changing `satp`. From this point on, the only data `userret` can use is the register contents and the content of the trapframe. `userret` loads the `TRAPFRAME` address into `a0`, restores saved user registers from the trapframe via `a0`, restores the saved user `a0`, and executes `sret` to return to user space.

`uservec` and `userret` are written in assembly language because it is difficult to write C code to save or restore all the registers or survive switching page tables.

## Code: Calling system calls

User programs call library functions in order to make system calls. For example, the shell displays a prompt with this function call (in `user/sh.c`):

      write(2, "$ ", 2);

Here's the library function, in `user/usys.S`:

    write:
     li a7, SYS_write
     ecall
     ret

The code that the C compiler generates for the function call loads the three arguments into registers `a0`, `a1`, and `a2`. Then the `write()` function loads the system call number, `SYS_write` (16), into `a7`. The kernel will look at those registers to find out what system call is intended, and what the arguments are. The `ecall` instruction traps from user space into the kernel and causes `uservec`, `usertrap`, and then `syscall` to execute.

At this point, please read [`kernel/syscall.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/syscall.c), `sys_write()` in [`kernel/sysfile.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c), and `copyout()`, `copyin()`, and `copyinstr()` in [`kernel/vm.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c).

[`syscall`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/syscall.c#L137) retrieves the system call number from the saved `a7` in the trapframe and uses it to index into [`syscalls`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/syscall.c#L109). For our example, `a7` contains [`SYS_write`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/syscall.h#L17), resulting in a call to the system call implementation function `sys_write`.

When `sys_write` returns, `syscall` records its return value in `p->trapframe->a0`. This will cause the original user-space call to `write()` to return that value, since the C calling convention on RISC-V places return values in `a0`. System calls conventionally return negative numbers to indicate errors, and zero or positive numbers for success.

## Code: System call arguments

System call arguments start out in the user registers, and are then moved to the trap frame by the kernel trap code. The kernel functions `argint`, `argaddr`, and `argfd` retrieve the *n* 'th system call argument from the trap frame as an integer, pointer, or a file descriptor.

Some system calls pass pointers as arguments, and the kernel must use those pointers to read or write user memory. The `write` system call, for example, passes the kernel a user-space pointer to the data to be written. Such pointers pose two challenges. First, the user program may be buggy or malicious, and may pass the kernel an invalid pointer or a pointer intended to trick the kernel into accessing kernel memory instead of user memory. Second, the xv6 kernel page table mappings are not the same as the user page table mappings, so the kernel cannot use ordinary instructions to load or store from user-supplied addresses.

The kernel implements functions that safely transfer data to and from user-supplied addresses. `fetchstr` is an example [(syscall.c:26)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/syscall.c#L26). File system calls such as `exec` use `fetchstr` to retrieve string file-name arguments from user space. `fetchstr` calls `copyinstr` to do the hard work.

[`copyinstr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c#L412) copies up to `max` bytes to `dst` from virtual address `srcva` in the user page table `pagetable`. Since `pagetable` is *not* the current page table, `copyinstr` uses `walkaddr` (which calls `walk`) to look up `srcva` in `pagetable`, yielding physical address `pa0`. The kernel's page table maps all of physical RAM at virtual addresses that are equal to the RAM's physical address. This allows `copyinstr` to directly copy string bytes from `pa0` to `dst`. [`walkaddr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c#L122) checks that the user-supplied virtual address is part of the process's user address space, so programs cannot trick the kernel into reading other memory. A similar function, `copyout`, copies data from the kernel to a user-supplied address.

<a id="s:ktraps"></a>

## Traps from kernel space
Please read [`kernel/kernelvec.S`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kernelvec.S), and `kerneltrap()` in [`kernel/trap.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c).

Xv6 handles traps from kernel code in a different way than traps from user code. When entering the kernel, `usertrap` points `stvec` to the assembly code at [`kernelvec`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kernelvec.S#L12). Since `kernelvec` only executes if xv6 was already in the kernel, `kernelvec` can rely on `satp` being set to the kernel page table, and on the stack pointer referring to a valid kernel stack. `kernelvec` pushes all caller-saved registers onto the current stack, from which it will later restore them so that the interrupted kernel code can resume without disturbance.

`kernelvec` saves the registers on the stack of the interrupted kernel thread, which makes sense because the register values belong to that thread. This is particularly important if the trap causes a switch to a different thread -- in that case the trap will actually return from the stack of the new thread, leaving the interrupted thread's saved registers safely on its stack.

`kernelvec` jumps to [`kerneltrap`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L137) after saving registers. `kerneltrap` is prepared for just one type of trap: device interrupts. It calls [`devintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L188) to handle them. If the trap isn't a device interrupt, it must be an exception, such as kernel code trying to use an invalid pointer. This could only be caused by a bug in the kernel code. The kernel does not have a way to recover in this situation, so it calls `panic()`, which prints an error message and then halts.

If `kerneltrap` was called due to a timer interrupt, and a process's kernel thread is running (as opposed to a scheduler thread), `kerneltrap` calls `yield` to give other threads a chance to run. At some point one of those threads will yield, and let our thread and its `kerneltrap` resume again. Chapter [8](/book/chapter-8/#CH:SCHED) explains what happens in `yield`.

When `kerneltrap`'s work is done, it needs to return to whatever code was interrupted by the trap. Because a `yield` may have disturbed `sepc` and the previous mode in `sstatus`, `kerneltrap` saves them when it starts. It now restores those control registers and returns to [`kernelvec`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kernelvec.S#L38). `kernelvec` pops the saved registers from the stack and executes `sret`, which copies `sepc` to `pc` and resumes the interrupted kernel code.

Xv6 sets a CPU's `stvec` to `kernelvec` when that CPU enters the kernel from user space; you can see this in [`usertrap`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L47). But there's a window of time when the kernel has started executing but `stvec` is still set to `uservec`, and it's crucial that no device interrupt occur during that window. Luckily the RISC-V always disables interrupts when it starts to take a trap, and `usertrap` doesn't enable them again until after it sets `stvec`.

## Real world

The need for trampoline pages could be eliminated if kernel memory were mapped into every process's user page table (with `PTE_U` clear). That would also eliminate the need for a page table switch when trapping from user space into the kernel. That in turn would allow system call implementations in the kernel to take advantage of the current process's user memory being mapped, allowing kernel code to directly dereference user pointers. Many operating systems have used these ideas to increase efficiency. Xv6 avoids them in order to reduce the chances of security bugs in the kernel due to inadvertent use of user pointers, and to reduce some complexity that would be required to ensure that user and kernel virtual addresses don't overlap.

## Exercises

1.  Could some or all of the code in `trampoline.S` and `kernelvec.S` be written in C rather than assembler?

2.  Is there a way to eliminate the special `TRAPFRAME` page mapping in every user address space? For example, could `uservec` be modified to simply push the 32 user registers onto the kernel stack, or store them in the `proc` structure?

3.  Could xv6 be modified to eliminate the special `TRAMPOLINE` page mapping?

## References
<span id="ref-riscv:priv"></span>
Waterman, Andrew, Krste Asanovic, and John Hauser, eds. 2024. *The RISC-V Instruction Set Manual Volume II: Privileged Specification*. [Https://drive.google.com/file/d/1uviu1nH-tScFfgrovvFCrj7Omv8tFtkp/view?usp=drive_link](https://drive.google.com/file/d/1uviu1nH-tScFfgrovvFCrj7Omv8tFtkp/view?usp=drive_link).
