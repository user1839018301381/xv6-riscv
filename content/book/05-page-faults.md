---
title: "Page faults"
chapter: 5
source: "xv6-riscv-book"
sourceFile: "pgfault.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
The RISC-V CPU raises a page-fault exception when a virtual address is used that has no mapping in the page table, or has a mapping whose `PTE_V` flag is clear, or a mapping whose permission bits (`PTE_R`, `PTE_W`, `PTE_X`, `PTE_U`) forbid the operation being attempted. RISC-V distinguishes three kinds of page fault: load page faults (caused by load instructions), store page faults (caused by store instructions), and instruction page faults (caused by fetches of instructions to be executed). The `scause` register indicates the type of the page fault and the `stval` register contains the address that couldn't be translated.

The combination of page tables and page faults is a powerful tool. Page tables give the kernel a level of indirection between virtual and physical addresses, so that the kernel can control the structure and content of address spaces. Page faults allow the kernel to intercept loads and stores and, by modifying the page table, specify on the fly what data those references refer to. The kernel can use these capabilities to increase efficiency: for example, copy-on-write fork allows the kernel to transparently share memory between parent and child, avoiding the cost of copying pages that neither write. Application programmers can also benefit. One possibility is memory-mapped files, where the kernel uses paging to cause a file's content to appear in an application's address space, transparently reading file blocks in response to page faults. Another is lazy memory allocation, which allows a program to ask for a huge virtual address space, but only to pay the cost of allocating physical memory for the pages the program actually reads and writes. xv6 uses page faults for only one purpose: lazy allocation.

Before proceeding, please read the functions `sys_sbrk()` in [`kernel/sysproc.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysproc.c), and `vmfault` in [`kernel/vm.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c). Search for calls to `vmfault` in [`kernel/trap.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c) and [`kernel/vm.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c).

<a id="sec:lazy"></a>

## Lazy allocation
xv6's *lazy allocation* has two parts. First, when an application asks for memory by calling `sbrk` with the flag `SBRK_LAZY`, the kernel notes the increase in size, but does not allocate physical memory and does not create PTEs for the new range of virtual addresses. Second, on a page fault on one of those new addresses, the kernel allocates a page of physical memory and maps it into the page table. The kernel implements lazy allocation transparently to applications: no modifications to applications are necessary for them to benefit.

Lazy allocation is convenient for applications because they don't have to accurately predict how much memory they will need. For example, an application may process input, but not know in advance how large the input will be. With lazy allocations an application can ask for memory for the worst case, but not have to pay for this worst case: the kernel doesn't have to do any work for pages that the application never uses, and other applications can allocate the unused pages.

Furthermore, if the application is asking to grow the address space by a lot, then `sbrk` without lazy allocation is expensive: if an application asks for a gigabyte of memory, the kernel has to allocate and zero 262,144 4096-byte physical pages. Lazy allocation allows this cost to be spread over time. On the other hand, lazy allocation incurs the extra overhead of page faults, which involve a user/kernel transition. Operating systems can reduce this cost by allocating a batch of consecutive pages per page fault instead of one page and by specializing the kernel entry/exit code for such page-faults (though xv6 does neither).

On the other hand, when taking a page fault for a lazily-allocated page, the kernel may find that it has not free memory to allocate. In this case, the kernel has no easy way of returning an out-of-memory error to the application and instead kills the application. For applications that prefer an error on a failed allocation, xv6 allows an application to allocate memory eagerly by calling `sbrk` with the flag `SBRK_EAGER`.

## Code

The system call `sbrk(n)` grows (or shrinks if `n` is negative) a process's memory size by `n` bytes, and returns the start of the newly allocated region (i.e., the old size). The kernel implementation is [`sys_sbrk`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysproc.c#L40).

If the application specifies `SBRK_EAGER`, the system call is implemented by the function [`growproc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c#L236). `growproc` calls `uvmalloc`. [`uvmalloc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c#L218) allocates physical memory with `kalloc`, zeros the allocated memory, and adds PTEs to the user page table with `mappages`.

If the applications allocates memory lazily, `sys_sbrk` just increments the process's size (`myproc()->sz`) by `n` and returns the old size; it does not allocate physical memory or add PTEs to the process's page table.

When a process loads or stores to a virtual address that lacks a valid page-table mapping, the CPU will raise *page-fault exception*. `usertrap` checks for this case [(trap.c:74)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c#L74) and calls [`vmfault`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c#L459) to handle the page fault. `vmfault` checks that the faulting address is within the region previously granted by `sbrk`, allocates a page of physical memory with `kalloc`, zeros the allocated page, and adds a PTE to the user page table with `mappages`. Xv6 sets the `PTE_W`, `PTE_R`, `PTE_U`, and `PTE_V` flags in the PTE for the new page. Then, `usertrap` resumes the process at the instruction that caused the fault. Because the PTE is now valid, the re-executed load or store instruction will execute without a fault.

If an application frees memory by calling `sbrk` with a negative `n`, `sys_sbrk` calls `growproc`, which calls `uvmdealloc`. The real work is done by [`uvmunmap`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c#L194), which uses `walk` to find PTEs. Since some pages may never have been used by the process and thus never have been allocated by `vmfault`, `uvmunmap` skips PTEs without the `PTE_V` flag. If a PTE mapping is valid, `uvmunmap` calls `kfree` to free the physical memory it refers to.

Note that Xv6 uses a process's page table not just to tell the hardware how to map user virtual addresses, but also as the only record of which physical memory pages are allocated to that process. That is the reason why freeing user memory (in `uvmunmap`) requires examination of the user page table.

## Real world: Copy-On-Write (COW) fork

Many kernels (though not xv6) use page faults to implement *copy-on-write (COW) fork*. The `fork` system call promises that the child sees memory whose initial content is the same as the parent's memory at the time of the fork. One way to implement this is to copy the entire memory of the parent to newly allocated physical memory for the child; this is what xv6 does. Copying can be slow, and it would be more efficient if the child could share the parent's physical memory. A straightforward implementation of this would not work, however, since it would cause the parent and child to disrupt each other's execution with their writes to the shared stack and heap.

Copy-on-write fork causes parent and child to safely share physical memory by appropriate use of page-table permissions and page faults. The basic plan is for the parent and child to initially share all physical pages, but for each to map them read-only (with the `PTE_W` flag clear). Parent and child can then read from the shared physical memory. If either writes a shared page, the RISC-V CPU raises a page-fault exception. A kernel supporting COW would respond by allocating a new page of physical memory and copying the shared page into that new page. Then kernel would change the relevant PTE in the faulting process's page table to point to the copy and to allow writes as well as reads, and then resume the faulting process at the instruction that caused the fault. Because the PTE now allows writes, the re-executed store instruction will execute without a fault, and will modify a private copy of the page rather than the shared page.

Copy-on-write requires book-keeping to help decide when physical pages can be freed, since each page can be referenced by a varying number of page tables depending on the history of forks, page faults, execs, and exits. This book-keeping allows an important optimization: if a process incurs a store page fault and the physical page is only referred to from that process's page table, no copy is needed.

Copy-on-write makes `fork` faster, since `fork` need not copy memory. Some of the memory will have to be copied later, when written, but it's often the case that most of the memory never has to be copied. A common example is `fork` followed by `exec`: a few pages may be written after the `fork`, but then the child's `exec` releases the bulk of the memory inherited from the parent. Copy-on-write `fork` eliminates the need to ever copy this memory. Furthermore, COW fork is transparent: no modifications to applications are necessary for them to benefit.

## Real world: Demand paging

Yet another widely-used feature that exploits page faults is *demand paging*. In the `exec` system call, xv6 loads all of an application's text and data into memory before starting the application. Since applications can be large and reading from disk takes time, this startup cost can be noticeable to users. To decrease startup time, a modern kernel doesn't initially load the executable file into memory, but just creates the user page table with all PTEs marked invalid. The kernel starts the program running; each time the program uses a page for the first time, a page fault occurs, and in response the kernel reads the content of the page from disk and maps it into the user address space. Like COW fork and lazy allocation, the kernel can implement this feature transparently to applications.

The programs running on a computer may need more memory than the computer has RAM. To cope gracefully, the operating system may implement *paging to disk*. The idea is to store only a fraction of user pages in RAM, and to store the rest on disk in a *paging area*. The kernel marks PTEs that correspond to memory stored in the paging area (and thus not in RAM) as invalid. If an application tries to use one of the pages that has been *paged out* to disk, the application will incur a page fault, and the page must be *paged in*: the kernel trap handler will allocate a page of physical RAM, read the page from disk into the RAM, and modify the relevant PTE to point to the RAM.

What happens if a page needs to be paged in, but there is no free physical RAM? In that case, the kernel must first free a physical page by paging it out or *evicting* it to the paging area on disk, and marking the PTEs referring to that physical page as invalid. Eviction is expensive, so paging performs best if it's infrequent: if applications use only a subset of their memory pages and the union of the subsets fits in RAM. This property is often referred to as having good locality of reference. As with many virtual memory techniques, kernels usually implement paging to disk in a way that's transparent to applications.

Computers often operate with little or no *free* physical memory, regardless of how much RAM the hardware provides. For example, cloud providers multiplex many customers on a single machine to use their hardware cost-effectively. As another example, users run many applications on smart phones in a small amount of physical memory. In such settings allocating a page may require first evicting an existing page. Thus, when free physical memory is scarce, allocation is expensive.

Lazy allocation and demand paging are particularly advantageous when free memory is scarce and programs actively use only a fraction of their allocated memory. These techniques can also avoid the work wasted when a page is allocated or loaded but either never used or evicted before it can be used.

## Real world: Memory-mapped files

Other features that combine paging and page-fault exceptions include automatically extending stacks and *memory-mapped files*, which are files that a program maps into its address space using the `mmap` system call so that the program can read and write them using load and store instructions.

## Exercises

1.  Write a user program that grows its address space by one byte by calling `sbrk(1)`. Run the program and investigate the page table for the program before the call to `sbrk` and after the call to `sbrk`. How much space has the kernel allocated? What does the PTE for the new memory contain?

2.  Implement COW fork.

3.  Implement `mmap`.
