---
title: "Interrupts and device drivers"
chapter: 6
source: "xv6-riscv-book"
sourceFile: "interrupt.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
A *driver* is the code in an operating system that manages a particular device: it configures the device hardware, tells the device to perform operations, handles the resulting interrupts, and interacts with processes using the device. Driver code can be tricky because a driver executes concurrently with the device, and often concurrently with processes using the device. In addition, the driver must understand the device's hardware interface, which can be complex and poorly documented.

Devices that need attention from the operating system can usually be configured to generate interrupts, which are one type of trap. The kernel trap handling code recognizes when a device has raised an interrupt and calls the driver's interrupt handler; in xv6, this dispatch happens in [`devintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L188).

Many device drivers execute code in two contexts: a *bottom half* that runs in a process's kernel thread, and a *top half* that executes at interrupt time. The bottom half is called via system calls such as `read` and `write` that want the device to perform I/O. This code may ask the hardware to start an operation (e.g., ask the disk to read a block); then the code waits for the operation to complete. Eventually the device completes the operation and raises an interrupt. The driver's interrupt handler, acting as the top half, figures out what operation has completed, wakes up a waiting process if appropriate, and tells the hardware to start work on the next operation, if any.

## Code: Console input

The console driver [(console.c)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c) is a simple illustration of driver structure. The console driver accepts characters typed by a human, via the *UART* serial-port hardware attached to the RISC-V. The console driver accumulates a line of input at a time, processing special input characters such as backspace and control-u. User processes, such as the shell, use the `read` system call to fetch lines of input from the console. When you type input to xv6 in QEMU, your keystrokes are delivered to xv6 by way of QEMU's simulated UART hardware.

The UART hardware that the driver talks to is a 16550 chip (Michael and Durich 1987) emulated by QEMU. On a real computer, a 16550 would manage an RS232 serial link connecting to a terminal or other computer. When running QEMU, it's connected to your keyboard and display.

The UART hardware appears to software as a set of *memory-mapped* control registers. That is, there are some physical addresses that are connected to the UART device, so that loads and stores interact with the device hardware rather than RAM. The memory-mapped addresses for the UART start at 0x10000000, or [`UART0`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/memlayout.h#L21). There are a handful of UART control registers, each the width of a byte. Their offsets from `UART0` are defined in [(uart.c:25)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c#L25). For example, the `LSR` register contains bits that indicate whether input characters are waiting to be read by the driver. These characters (if any) are available for reading from the `RHR` register. Each time one is read, the UART hardware deletes it from an internal FIFO of waiting characters, and clears the "ready" bit in `LSR` when the FIFO is empty. To transmit, the driver writes a byte to the `THR` register, which causes the UART to append the byte to a FIFO of bytes that the UART will send on the RS232 serial link. The UART transmit and receive hardware are largely independent of each other.

Xv6's `main` calls [`consoleinit`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c#L193) to initialize the UART hardware. This code configures the UART to generate a receive interrupt when the UART receives each byte of input, and a *transmit complete* interrupt each time the UART finishes sending a byte of output [(uart.c:49)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c#L49).

The xv6 shell reads from the console by way of a file descriptor opened by [`init.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/init.c#L19). Calls to the `read` system call make their way through the kernel to [`consoleread`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c#L88). `consoleread` waits for input to arrive (via interrupts) and be buffered in `cons.buf`, copies the input to user space, and (after a whole line has arrived) returns to the user process. If the user hasn't typed a full line yet, any reading processes will wait in the `sleep` call [(console.c:106)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c#L106) (Chapter [9](/book/chapter-9/#CH:SLEEP) explains the details of `sleep`).

When the user types a character, the UART hardware asks the RISC-V to raise an interrupt, which activates xv6's trap handler. The trap handler calls [`devintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L188), which looks at the RISC-V `scause` register to discover that the interrupt is from an external device. Then it asks a hardware unit called the PLIC  (Waterman et al. 2024) to tell it which device interrupted [(trap.c:196)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L196). If it was the UART, `devintr` calls `uartintr`.

[`uartintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c#L139) reads any waiting input characters from the UART hardware and hands them to [`consoleintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c#L147); it doesn't wait for characters, since future input will raise a new interrupt. The job of `consoleintr` is to accumulate input characters in `cons.buf` until a whole line arrives. `consoleintr` treats backspace and a few other characters specially. When a newline arrives, `consoleintr` wakes up a waiting `consoleread` (if there is one).

Once woken, `consoleread` will observe a full line in `cons.buf`, copy it to user space, and return (via the system call machinery) to user space.

A pattern to note is the decoupling of device activity from process activity via buffering and interrupts. The console driver can process input even when no process is waiting to read it; a subsequent read will see the input. This decoupling can increase performance by allowing processes to execute concurrently with device I/O, and is particularly important when the device is slow (as with the UART) or needs immediate attention (as with echoing typed characters). This idea is sometimes called *I/O concurrency*.

## Code: Console output

A `write` system call on a file descriptor connected to the console reaches [`consolewrite`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/console.c#L63), which copies batches of bytes from user space and hands each batch to [`uartwrite`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c#L80). Before writing each byte to the UART's THR register, `uartwrite` must wait for the UART to be ready to accept more output. Because the UART is relatively slow, `uartwrite` waits using `sleep` (which yields the CPU) rather than a busy-loop. When the UART is done sending the most recent byte, it interrupts. The interrupt routine, [`uartintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c#L139), cooperates with `uartwrite`: before sending each byte, `uartwrite` reads the UART's LSR register to see whether the UART can accept another byte, and calls `sleep` if it cannot; the interrupt routine calls `wakeup` when the UART signals it is ready for more output, which causes `uartwrite` to wake up, re-check the LSR register, and send the next byte.

## Concurrency in drivers

You may have noticed calls to `acquire` in `consoleread` and in `consoleintr`. These calls acquire a lock, which protects the console driver's data structures from concurrent access. There are three concurrency dangers here: two processes on different CPUs might call `consoleread` at the same time; the hardware might ask a CPU to deliver a console (really UART) interrupt while that CPU is already executing inside `consoleread`; and the hardware might deliver a console interrupt on a different CPU while `consoleread` is executing. Chapter [7](/book/chapter-7/#CH:LOCK) explains how to use locks to ensure that these dangers don't lead to incorrect results.

Another way in which concurrency requires care in drivers is that one process may be waiting for input from a device, but the interrupt signaling arrival of the input may arrive when a different process (or no process at all) is running. Thus interrupt handlers are not allowed to think about the process or code that they have interrupted. For example, an interrupt handler cannot safely call `copyout` with the current process's page table. Interrupt handlers typically do relatively little work (e.g., just copy the input data to a buffer), and wake up bottom-half code to do the rest.

## Timer interrupts

Xv6 uses timer interrupts to maintain its idea of the current time and to switch among compute-bound processes. Timer interrupts come from clock hardware attached to each RISC-V CPU. Xv6 programs each CPU's clock hardware to interrupt the CPU periodically.

Code in [`start.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/start.c#L56) sets some control bits that allow supervisor-mode access to the timer control registers, and then asks for the first timer interrupt. The `time` control register contains a count that the hardware increments at a steady rate; this serves as a notion of the current time. The `stimecmp` register contains a time at which the CPU will raise a timer interrupt; setting `stimecmp` to the current value of `time` plus *x* will schedule an interrupt *x* time units in the future. For `qemu`'s RISC-V emulation, 1000000 time units is roughly a tenth of second.

Timer interrupts arrive via `usertrap` or `kerneltrap` and `devintr`, like other device interrupts. Timer interrupts arrive with `scause`'s low bits set to five; `devintr` in `trap.c` detects this situation and calls [`clockintr`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L167). The latter function increments `ticks`, allowing the kernel to track the passage of time. The increment occurs on only one CPU, to avoid time passing faster if there are multiple CPUs. `clockintr` wakes up any processes waiting in the `pause` system call, and schedules the next timer interrupt by writing `stimecmp`.

`devintr` returns 2 for a timer interrupt in order to indicate to `kerneltrap` or `usertrap` that they should call `yield` so that CPUs can be multiplexed among runnable processes.

The fact that kernel code can be interrupted by a timer interrupt that forces a context switch via `yield` is part of the reason why early code in `usertrap` is careful to save state such as `sepc` before enabling interrupts. These context switches also mean that kernel code must be written in the knowledge that it may move from one CPU to another without warning.

## Real world

Xv6, like many operating systems, allows interrupts and even context switches (via `yield`) while executing in the kernel. The reason for this is to retain quick response times during complex system calls that run for a long time. However, as noted above, allowing interrupts in the kernel is the source of some complexity; as a result, a few operating systems allow interrupts only while executing user code.

Supporting all the devices on a typical computer in its full glory is much work, because there are many devices, the devices have many features, and the protocol between device and driver can be complex and poorly documented. In many operating systems, the drivers account for more code than the core kernel.

The UART driver retrieves data a byte at a time by reading the UART control registers; this pattern is called *programmed I/O*, since software is driving the data movement. Programmed I/O is simple, but too slow to be used at high data rates. Devices that need to move lots of data at high speed typically use *direct memory access (DMA)*. DMA device hardware directly writes incoming data to RAM, and reads outgoing data from RAM. Modern disk and network devices use DMA. A driver for a DMA device would prepare data in RAM, and then use a single write to a control register to tell the device to process the prepared data.

Interrupts make sense when a device needs attention at unpredictable times, and not too often. But interrupts have high CPU overhead. Thus high speed devices, such as network and disk controllers, use tricks that reduce the need for interrupts. One trick is to raise a single interrupt for a whole batch of incoming or outgoing requests. Another trick is for the driver to disable interrupts entirely, and to check the device periodically to see if it needs attention. This technique is called *polling*. Polling makes sense if the device performs operations at a high rate, but it wastes CPU time if the device is mostly idle. Some drivers dynamically switch between polling and interrupts depending on the current device load.

The UART driver copies incoming data first to a buffer in the kernel, and then to user space. This makes sense at low data rates, but such a double copy can significantly reduce performance for devices that generate or consume data very quickly. Some operating systems are able to directly move data between user-space buffers and device hardware, often with DMA.

As mentioned in Chapter [1](/book/chapter-1/#CH:UNIX), the console appears to applications as a regular file, and applications read input and write output using the `read` and `write` system calls. Applications may want to control aspects of a device that cannot be expressed through the standard file system calls (e.g., enabling/disabling line buffering in the console driver). Unix operating systems provide an `ioctl` system call for such cases.

Some uses of computers require "real-time" responses to external events: responses guaranteed to occur within a bounded time. For example, in safety-critical systems missing a deadline can lead to disasters. Xv6 is not suitable for real-time settings. Among other things, xv6's scheduler does not take into account real-time deadlines when it decides what process to run next, and xv6 has long kernel code paths with interrupts disabled, so that it may not respond to interrupts quickly. A real-time operating system must not only fix these problems, but also be structured in a way that allows analysis of worst-case response times.

## Exercises

1.  Modify `uart.c` to not use interrupts at all. You may need to modify `console.c` as well.

2.  Add a driver for an Ethernet card.

## References
<span id="ref-ns16550a"></span>
Michael, Martin, and Daniel Durich. 1987. *The NS16550A: UART Design and Application Considerations*. [Http://bitsavers.trailing-edge.com/components/national/\_appNotes/AN-0491.pdf](http://bitsavers.trailing-edge.com/components/national/_appNotes/AN-0491.pdf).

<span id="ref-riscv:priv"></span>
Waterman, Andrew, Krste Asanovic, and John Hauser, eds. 2024. *The RISC-V Instruction Set Manual Volume II: Privileged Specification*. [Https://drive.google.com/file/d/1uviu1nH-tScFfgrovvFCrj7Omv8tFtkp/view?usp=drive_link](https://drive.google.com/file/d/1uviu1nH-tScFfgrovvFCrj7Omv8tFtkp/view?usp=drive_link).
