---
title: "Operating system organization"
chapter: 2
source: "xv6-riscv-book"
sourceFile: "first.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
A key requirement for an operating system is to support several activities at once. For example, one might use the `fork` and `exec` system calls from Chapter [1](/book/chapter-1/#CH:UNIX) to start both a compiler and a text editor as processes. The operating system must *time-share* resources such as CPUs and memory among these processes. The operating system must also arrange for *isolation* between the processes. If one process has a bug and malfunctions, it shouldn't affect unrelated processes. Complete isolation, however, is too strong, since it should be possible for processes to intentionally interact; pipelines are an example. Thus an operating system must fulfill three requirements: multiplexing, isolation, and interaction.

This chapter provides an overview of how operating systems are organized to achieve these three requirements. It turns out there are many ways to do so, but this text focuses on mainstream designs centered around a *monolithic kernel*, which is used by many Unix operating systems. This chapter also provides an overview of an xv6 process, the unit of isolation in xv6.

Xv6 runs on a *multi-core*[^1] RISC-V microprocessor, and much of its low-level functionality (for example, its process implementation) is specific to RISC-V. RISC-V is a 64-bit CPU, and xv6 is written in "LP64" C, which means long (L) and pointers (P) in the C programming language are 64 bits, but an `int` is 32 bits. This book assumes the reader has done a bit of machine-level programming on some architecture, and will introduce RISC-V-specific ideas as they come up. The user-level ISA (Waterman and Asanovic 2024) and privileged architecture (Waterman et al. 2024) documents are the complete specifications. You may also refer to "The RISC-V Reader: An Open Architecture Atlas" (Patterson and Waterman 2017).

The CPU in a complete computer is surrounded by support hardware, much of it in the form of I/O interfaces. Xv6 is written for the support hardware simulated by qemu's "-machine virt" option. This includes RAM, a ROM containing boot code, a serial connection to the user's keyboard/screen, and a disk for storage.

## Abstracting physical resources

The first question one might ask when encountering an operating system is why have it at all? That is, one could implement the system calls in Figure [1.2](#fig:api) as a library, with which applications link. In this plan, each application could even have its own library tailored to its needs. Applications could directly interact with hardware resources and use those resources in the best way for the application (e.g., to achieve high or predictable performance). Some operating systems for embedded devices or real-time systems are organized in this way.

The downside of this library approach is that, if there is more than one application running, the applications must be well-behaved. For example, each application must periodically give up the CPU so that other applications can run. Such a *cooperative* time-sharing scheme may be OK if all applications trust each other and have no bugs. It's more typical for applications to not trust each other, and to have bugs, so one often wants stronger isolation than a cooperative scheme provides.

To achieve strong isolation it's helpful to forbid applications from directly accessing sensitive hardware resources, and instead to abstract the resources into services. For example, Unix applications interact with storage only through the file system's `open`, `read`, `write`, and `close` system calls, instead of reading and writing the disk directly. This provides the application with the convenience of pathnames, and it allows the operating system (which provides the interface) to manage the disk. Even if isolation is not a concern, programs that interact intentionally (or just wish to keep out of each other's way) are likely to find a file system a more convenient abstraction than direct use of the disk.

Similarly, Unix transparently switches hardware CPUs among processes, saving and restoring register state as necessary, so that applications don't have to be aware of time-sharing. This transparency allows the operating system to share CPUs even if some applications are in infinite loops.

As another example, Unix processes use `exec` to build up their memory image, instead of directly interacting with physical memory. This allows the operating system to decide where to place a process in memory; if memory is tight, the operating system might even store some of a process's data on disk. `exec` also provides users with the convenience of a file system to store executable program images.

Many forms of interaction among Unix processes occur via file descriptors. Not only do file descriptors abstract away many details (e.g., where data in a pipe or file is stored), they are also defined in a way that simplifies interaction. For example, if one application in a pipeline exits or fails, the kernel automatically generates an end-of-file signal for the next process in the pipeline.

The system-call interface in Figure [1.2](#fig:api) is carefully designed to provide both programmer convenience and the possibility of strong isolation. The Unix interface is not the only way to abstract resources, but it has proved to be a good one.

## User mode, supervisor mode, and system calls

Strong isolation requires a hard boundary between applications and the operating system. Applications shouldn't be allowed to disturb the operation of the operating system or other programs, even if the application has a bug or is malicious. To achieve strong isolation, the operating system must arrange that applications cannot modify (or even read) the operating system's data structures and instructions and that applications cannot access other processes' memory.

CPUs provide hardware support for strong isolation. For example, RISC-V has three privilege levels which constrain what code can do: *machine mode*, *supervisor mode*, and *user mode*. Instructions executing in machine mode have full privilege; a CPU starts in machine mode. Machine mode is mostly intended for setting up the computer during boot. Xv6 executes briefly in machine mode and then changes to supervisor mode.

In supervisor mode the CPU is allowed to execute *privileged instructions*: for example, enabling and disabling interrupts, reading and writing the register that holds the address of the page table, etc. If an application in user mode attempts to execute a privileged instruction, then the CPU doesn't execute the instruction, but "traps" to special code in supervisor mode that can terminate the application. Figure [1.1](#fig:os) in Chapter [1](/book/chapter-1/#CH:UNIX) illustrates this organization. An application can execute only user-mode instructions (e.g., adding numbers, etc.) and is said to be running in *user space*, while the software in supervisor mode can also execute privileged instructions and is said to be running in *kernel space*. The software running in kernel space (or in supervisor mode) is called the *kernel*.

Applications interact with the kernel via system calls such as `read`. Applications are not allowed to directly call kernel functions or access the kernel's memory. RISC-V provides the `ecall` instruction for system calls; it switches the CPU from user to supervisor mode and jumps to a kernel-specified entry point. Once the CPU has switched to supervisor mode, the kernel can then validate the arguments of the system call (e.g., check if the address passed to the system call is part of the application's memory), decide whether the application is allowed to perform the requested operation (e.g., check if the application is allowed to write the specified file), and then deny it or execute it. It is important that the kernel control the entry point for transitions to supervisor mode; if the application could decide the kernel entry point, a malicious application could, for example, enter the kernel at a point where the validation of arguments is skipped.

## Kernel organization

A key design question is what part of the operating system should run in supervisor mode. One possibility is that the entire operating system resides in the kernel, so that the implementations of all system calls run in supervisor mode. This organization is called a *monolithic kernel*.

In a monolithic organization the entire operating system consists of a single program running in supervisor mode. One reason this organization is convenient is that the OS designer doesn't have to divide code into parts that do and do not require supervisor privileges. Furthermore, it is easy for different parts of the operating system to cooperate, since they are parts of a single program. For example, a monolithic kernel might share a disk block cache with the file system and the virtual memory system.

A downside is that monolithic kernels tend to grow large and complex, so that no one developer understands all of the interactions between different parts of the code; this is a recipe for bugs. A bug in the kernel is particularly troublesome because it may cause the entire computer to crash, or cause many applications to malfunction, or make the entire computer vulnerable to security attacks.

A *microkernel* aims to reduce the incidence of bugs in the kernel. The idea is to put an absolute minimum of functionality in the kernel itself, so that little code executes in supervisor mode, and so that the kernel is easy to understand and analyze for correctness. The bulk of the operating system runs as user-level server processes. For example, the file system code would execute as a server process, in user mode rather than supervisor mode.

<figure id="fig:mkernel" data-latex-placement="t">
<img src="/book/fig/mkernel.svg" alt="" loading="lazy">
<figcaption>
Figure 2.1: A microkernel with a file-system server
</figcaption>
</figure>

Figure [2.1](#fig:mkernel) illustrates this microkernel design. In the figure, the file system runs as a user-level server process. To allow applications to interact with the file server, the kernel provides an inter-process communication mechanism to send messages from one user-mode process to another. For example, if an application like the shell wants to read or write a file, it sends a message to the file server and waits for a response.

In a microkernel, the kernel interface consists of a few low-level functions for starting applications, sending messages, accessing device hardware, etc. This organization allows the kernel to be relatively simple, as most of the operating system resides in user-level servers.

In the real world, both monolithic kernels and microkernels are popular. Many Unix kernels are monolithic. For example, Linux has a monolithic kernel, although some OS functions run as user-level servers (e.g., the window system). Linux delivers high performance to OS-intensive applications, partially because the subsystems of the kernel can be tightly integrated.

Operating systems such as Minix, L4, and QNX are organized as a microkernel with servers, and have seen wide deployment in embedded settings. A variant of L4, seL4, is small enough that it has been verified for memory safety and other security properties (Klein et al. 2009).

There is much debate among developers of operating systems about which organization is better, but there is no conclusive evidence one way or the other. Furthermore, it depends much on what "better" means: faster performance, smaller code size, reliability of the kernel, reliability of the complete operating system (including user-level services), etc.

There are also practical considerations that may be more important than the question of which organization. Some operating systems have a microkernel but run some of the user-level services in kernel space for performance reasons. Some operating systems have monolithic kernels because that is how they started and there is little incentive to move to a pure microkernel organization, because new features may be more important than rewriting the existing operating system to fit a microkernel design.

From this book's perspective, microkernel and monolithic operating systems share many key ideas. They implement system calls, they use page tables, they handle interrupts, they support processes, they use locks for concurrency control, they implement a file system, etc. This book focuses on these core ideas.

Xv6 is implemented as a monolithic kernel, like most Unix operating systems. Thus, the xv6 kernel interface corresponds to the operating system interface, and the kernel implements the complete operating system. Since xv6 doesn't provide many services, its kernel is smaller than some microkernels, but conceptually xv6 is monolithic.

## Code: xv6 organization

<figure id="fig:source" data-latex-placement="t">
<table>
<thead>
<tr>
<th style="text-align: left;">
</th>
<th style="text-align: left;">
<strong>File</strong>
</th>
<th style="text-align: left;">
<strong>Description</strong>
</th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;">
Boot
</td>
<td style="text-align: left;">
entry.S
</td>
<td style="text-align: left;">
Very first boot instructions.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
main.c
</td>
<td style="text-align: left;">
Control initialization of other modules.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
start.c
</td>
<td style="text-align: left;">
Early machine-mode boot code.
</td>
</tr>
<tr>
<td style="text-align: left;">
Processes
</td>
<td style="text-align: left;">
exec.c
</td>
<td style="text-align: left;">
exec() system call.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
proc.c
</td>
<td style="text-align: left;">
Processes and scheduling.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
swtch.S
</td>
<td style="text-align: left;">
Thread switching.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
sysproc.c
</td>
<td style="text-align: left;">
Process-related system calls.
</td>
</tr>
<tr>
<td style="text-align: left;">
Traps
</td>
<td style="text-align: left;">
kernelvec.S
</td>
<td style="text-align: left;">
Handle traps from kernel code.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
trampoline.S
</td>
<td style="text-align: left;">
Handle traps from user code.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
trap.c
</td>
<td style="text-align: left;">
C code to handle and return from traps and interrupts.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
syscall.c
</td>
<td style="text-align: left;">
Dispatch system calls to handling function.
</td>
</tr>
<tr>
<td style="text-align: left;">
Memory
</td>
<td style="text-align: left;">
vm.c
</td>
<td style="text-align: left;">
Manage page tables and address spaces.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
kalloc.c
</td>
<td style="text-align: left;">
Physical page allocator.
</td>
</tr>
<tr>
<td style="text-align: left;">
Devices
</td>
<td style="text-align: left;">
console.c
</td>
<td style="text-align: left;">
Connect to the user keyboard and screen.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
plic.c
</td>
<td style="text-align: left;">
RISC-V interrupt controller.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
printk.c
</td>
<td style="text-align: left;">
Formatted output to the console.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
uart.c
</td>
<td style="text-align: left;">
Serial-port console device driver.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
virtio_disk.c
</td>
<td style="text-align: left;">
Disk device driver.
</td>
</tr>
<tr>
<td style="text-align: left;">
FS
</td>
<td style="text-align: left;">
bio.c
</td>
<td style="text-align: left;">
Disk block cache for the file system.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
file.c
</td>
<td style="text-align: left;">
File descriptor support.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
fs.c
</td>
<td style="text-align: left;">
File system.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
log.c
</td>
<td style="text-align: left;">
File system logging and crash recovery.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
sysfile.c
</td>
<td style="text-align: left;">
File-related system calls.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
pipe.c
</td>
<td style="text-align: left;">
Pipes.
</td>
</tr>
<tr>
<td style="text-align: left;">
Misc
</td>
<td style="text-align: left;">
sleeplock.c
</td>
<td style="text-align: left;">
Locks that yield the CPU.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
spinlock.c
</td>
<td style="text-align: left;">
Locks that don't yield the CPU.
</td>
</tr>
<tr>
<td style="text-align: left;">
</td>
<td style="text-align: left;">
string.c
</td>
<td style="text-align: left;">
C string and byte-array library.
</td>
</tr>
</tbody>
</table>
<figcaption>
Figure 2.2: Xv6 kernel source files.
</figcaption>
</figure>

The xv6 kernel source is in the `kernel/` sub-directory. Figure [2.2](#fig:source) lists the files, divided into the major areas of kernel responsibility: starting the system (booting), creating and controlling processes, handling traps (interrupts and system calls), allocating memory and configuring virtual addresses, controlling devices, and managing the file-system.

## Process overview

The unit of isolation in xv6 (as in other Unix operating systems) is a *process*. The process abstraction prevents one process from wrecking or spying on another process's memory, CPU, file descriptors, etc. It also prevents a process from wrecking the kernel itself, so that a process can't subvert the kernel's isolation mechanisms. The kernel must implement the process abstraction with care because a buggy or malicious application may trick the kernel or hardware into doing something bad (e.g., circumventing isolation). The mechanisms used by the kernel to implement processes include the user/supervisor mode flag, address spaces, and time-slicing of threads.

To help enforce isolation, the process abstraction provides the illusion to a program that it has its own private machine. A process provides a program with what appears to be a private memory system, or *address space*, which other processes cannot read or write. A process also provides the program with what appears to be its own CPU to execute the program's instructions.

Xv6 uses page tables (which are implemented by hardware) to give each process its own address space. The RISC-V page table translates (or "maps") a *virtual address* (the address that an RISC-V instruction manipulates) to a *physical address* (an address that the CPU sends to main memory).

<figure id="fig:as" data-latex-placement="t">
<img src="/book/fig/as.svg" alt="" loading="lazy">
<figcaption>
Figure 2.3: Layout of a process's virtual address space
</figcaption>
</figure>

Xv6 maintains a separate page table for each process that defines that process's address space. As illustrated in Figure [2.3](#fig:as), an address space includes the process's *user memory* starting at virtual address zero. Instructions come first, followed by global variables, then the stack, and finally a "heap" area (for malloc) that the process can expand as needed. There are a number of factors that limit the maximum size of a process's address space: pointers on the RISC-V are 64 bits wide; the hardware uses only the low 39 bits when looking up virtual addresses in page tables; and xv6 uses only 38 of those 39 bits. Thus, the maximum address is $2^{38}-1$ = 0x3fffffffff, which is [`MAXVA`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/riscv.h#L417). At the top of the address space xv6 places a *trampoline* page (4096 bytes) and a *trapframe* page. Xv6 uses these two pages to transition into the kernel and back; the trampoline page contains the code to transition in and out of the kernel, and the trapframe is where the kernel saves the process's user registers, as Chapter [4](/book/chapter-4/#CH:TRAP) explains.

The xv6 kernel maintains many pieces of state for each process, which it gathers into a [`struct proc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.h#L82). A process's most important pieces of kernel state are its page table, its kernel stack, and its run state. We'll use the notation `p->xxx` to refer to elements of the `proc` structure; for example, `p->pagetable` is a pointer to the process's page table.

At this point, please read [`kernel/proc.h`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.h), which defines `struct proc`. The xv6 code is more important for you to understand than this book; you should prioritize the code, and consult this book as needed to clarify the code. The purpose of some of the code may not be apparent at first, but further reading and searching the code will help. Feel free to explore and modify the code.

Each process has a thread of control (or *thread* for short) that holds the state needed to execute the process. At any given time, a thread might be executing on a CPU, or suspended (not executing, but capable of resuming executing in the future). To switch a CPU between processes, the kernel suspends the thread currently running on that CPU and saves its state, and restores the state of another process's previously-suspended thread. Much of the state of a thread (local variables, function call return addresses) is stored on the thread's stacks. Each process has two stacks: a user stack and a kernel stack (`p->kstack`). When the process is executing user instructions, only its user stack is in use, and its kernel stack is empty. When the process enters the kernel (for a system call or interrupt), the kernel code executes on the process's kernel stack; while a process is in the kernel, its user stack still contains saved data, but isn't actively used. A process's thread alternates between actively using its user stack and its kernel stack. The kernel stack is separate (and protected from user code) so that the kernel can execute even if a process has wrecked its user stack.

A process's user code can make a system call by executing the RISC-V `ecall` instruction. This instruction switches to supervisor mode and changes the program counter to a kernel-defined entry point. The code at the entry point switches to the process's kernel stack and executes the kernel instructions that implement the system call. When the system call completes, the kernel returns to user space by executing the `sret` instruction, which switches to user mode and resumes executing user instructions just after the system call instruction. A process's thread can "block" in the kernel to wait for I/O, and resume where it left off when the I/O has finished.

`p->state` indicates whether the process is allocated, ready to run, currently running on a CPU, waiting for I/O, or exiting.

`p->pagetable` holds the process's page table, in the format that the RISC-V hardware expects. Xv6 causes the paging hardware to use a process's `p->pagetable` when executing that process in user space. A process's page table also serves as the record of the addresses of the physical pages allocated to store the process's memory.

In summary, a process bundles two design ideas: an address space to give a process the illusion of its own memory, and a thread to give the process the illusion of its own CPU. In xv6, a process consists of one address space and one thread. In real operating systems a process may have more than one thread to take advantage of multiple CPUs.

## Code: Starting xv6, the first process and system call

To make xv6 more concrete, we'll outline how the kernel starts and runs the first process. The subsequent chapters will describe the mechanisms that show up in this overview in more detail. Please read [`kernel/entry.S`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/entry.S), [`kernel/start.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/start.c), [`kernel/main.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/main.c), and [`user/init.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/init.c).

When the RISC-V computer powers on, it initializes itself and runs a boot loader which is stored in read-only memory. The boot loader copies the xv6 kernel into memory at physical address `0x80000000`. The reason it places the kernel at `0x80000000` rather than `0x0` is because the address range `0x0:0x80000000` contains I/O devices.

Then the boot loader jumps to xv6 starting at [`_entry`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/entry.S#L7). The RISC-V starts with paging hardware disabled: virtual addresses map directly to physical addresses. The instructions at `_entry` set up a stack so that xv6 can run C code. Xv6 declares space for this stack, `stack0`, in the file [`start.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/start.c#L11). The code at `_entry` loads the stack pointer register `sp` with the address `stack0+4096`, the top of the stack, because the stack on RISC-V grows down. Now that the kernel has a stack, `_entry` calls into C code at [`start`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/start.c#L15).

The function `start` performs some setup that the CPU only allows in machine mode, most crucially programming the clock chip to generate timer interrupts. Then `start` uses the RISC-V `mret` instruction to switch to supervisor mode and jump to [`main`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/main.c#L11). `mret` requires a bit of setup: `start` sets the previous privilege mode to supervisor in the register `mstatus`, sets the destination address to `main` by writing `main`'s address into the register `mepc`, disables virtual address translation in supervisor mode by writing `0` into the page-table register `satp`, and delegates all interrupts and exceptions to supervisor mode.

After [`main`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/main.c#L11) initializes several devices and subsystems, it creates the first process by calling [`userinit`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L219). All newly created processes start executing in the kernel in [`forkret`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L513). As a special case for the first process, `forkret` calls `kexec` to load the user program `/init`.

After calling `kexec`, `forkret` returns to user space in the `/init` process. [`init`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/init.c#L15) creates a new console device file if needed and then opens it as file descriptors 0, 1, and 2. Then it starts a shell on the console. The system is up.

## Security Model

You may wonder how the operating system deals with buggy or malicious code. Because coping with malice is strictly harder than dealing with accidental bugs, it's reasonable to focus mostly on providing security against malice. Here's a high-level view of typical security assumptions and goals in operating system design.

The operating system must assume that a process's user-level code will do its best to wreck the kernel or other processes. User code may try to dereference pointers outside its allowed address space; it may attempt to execute instructions not intended for user code; it may try to read and write RISC-V control registers; it may try to access device hardware; and it may pass clever values to system calls in an attempt to trick the kernel into crashing or doing something stupid.

The kernel's goal is to restrict each user process so that it can only access its own user memory, use the 32 general-purpose RISC-V registers, and affect the kernel and other processes in the ways that system calls are intended to allow. The kernel must prevent any other actions. These are typically absolute requirements in kernel design.

Expectations for the kernel's own code are different. Kernel code is assumed to be written by well-meaning and careful programmers, to be bug-free, and to contain nothing malicious. This assumption affects how we analyze kernel code. For example, there are many internal kernel functions (e.g., the spin locks) that would cause serious problems if kernel code used them incorrectly. We assume, however, that the kernel uses its own functions correctly. At the hardware level, the RISC-V CPU, RAM, disk, etc. are assumed to operate as advertised in the documentation, with no hardware bugs.

Real life is not so straightforward. It's difficult to prevent abusive user programs from calling system calls in a way that makes the system unusable by consuming kernel-protected resources: disk space, CPU time, process table slots, etc. It's usually impossible to write 100% bug-free kernel code or design bug-free hardware; if the writers of malicious user code are aware of kernel or hardware bugs, they will exploit them. Even in mature, widely-used kernels, such as Linux, people often discover previously-unknown vulnerabilities (*Linux Common Vulnerabilities and Exposures (CVEs)*, n.d.). Finally, the distinction between user and kernel code is sometimes blurred: some privileged user-level processes may provide essential services and effectively be part of the operating system, and in some operating systems privileged user code can insert new code into the kernel (as with Linux's loadable kernel modules and eBPF).

As a partial defense against kernel bugs, xv6 code includes checks for inconsistencies and unrecoverable errors, and will "panic" in response, by calling `panic()`. This function prints an error message and halts the system. Panicking is not desirable, but is preferable to continuing execution. Typically a panic results from a kernel bug that causes kernel data to be incorrect or causes the kernel to perform an illegal action such as referencing non-existent memory; in such a situation it is safer to halt execution with `panic()` than to try to continue in an inconsistent state. A kernel developer would react to a panic by working to identify and fix the underlying code bug.

## Real world

Most operating systems have adopted the process concept, and most processes look similar to xv6's. Modern operating systems, however, support several threads within a process, to allow a single process to exploit multiple CPUs. Supporting multiple threads in a process involves quite a bit of machinery that xv6 doesn't have, often including interface changes (e.g., Linux's `clone`, a variant of `fork`), to control which aspects of a process threads share.

## Exercises

1.  Add a system call to xv6 that returns the amount of free memory available.

## References
<span id="ref-sel4"></span>
Klein, Gerwin, Kevin Elphinstone, Gernot Heiser, et al. 2009. "SeL4: Formal Verification of an OS Kernel." *Proceedings of the ACM SIGOPS 22nd Symposium on Operating Systems Principles*, 207--20.

<span id="ref-mitre:cves"></span>
*Linux Common Vulnerabilities and Exposures (CVEs)*. n.d. [Https://cve.mitre.org/cgi-bin/cvekey.cgi?keyword=linux](https://cve.mitre.org/cgi-bin/cvekey.cgi?keyword=linux).

<span id="ref-riscv"></span>
Patterson, David, and Andrew Waterman. 2017. *The RISC-V Reader: An Open Architecture Atlas*. Strawberry Canyon.

<span id="ref-riscv:user"></span>
Waterman, Andrew, and Krste Asanovic, eds. 2024. *The RISC-V Instruction Set Manual Volume I: Unprivileged Specification ISA*. [Https://drive.google.com/file/d/17GeetSnT5wW3xNuAHI95-SI1gPGd5sJ\_/view?usp=drive_link](https://drive.google.com/file/d/17GeetSnT5wW3xNuAHI95-SI1gPGd5sJ_/view?usp=drive_link).

<span id="ref-riscv:priv"></span>
Waterman, Andrew, Krste Asanovic, and John Hauser, eds. 2024. *The RISC-V Instruction Set Manual Volume II: Privileged Specification*. [Https://drive.google.com/file/d/1uviu1nH-tScFfgrovvFCrj7Omv8tFtkp/view?usp=drive_link](https://drive.google.com/file/d/1uviu1nH-tScFfgrovvFCrj7Omv8tFtkp/view?usp=drive_link).


[^1]: By "multi-core" this text means multiple CPUs that share memory but execute in parallel, each with its own set of registers. This text sometimes uses the term *multiprocessor* as a synonym for multi-core, though multiprocessor can also refer more specifically to a computer with several distinct processor chips.
