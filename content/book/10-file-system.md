---
title: "File system"
chapter: 10
source: "xv6-riscv-book"
sourceFile: "fs.tex"
commit: "2d689eee47087bea267914123b8b1172583cbb0e"
---
The purpose of a file system is to organize and store data. File systems typically support sharing of data among users and applications, as well as *persistence* so that data is still available after a reboot.

The xv6 file system provides Unix-like files, directories, and pathnames (see Chapter [1](/book/chapter-1/#CH:UNIX)), and stores its data on a disk for persistence. The file system addresses several challenges:

- The file system needs on-disk data structures to represent the tree of named directories and files, to record the identities of the blocks that hold each file's content, and to record which areas of the disk are free.

- Different processes may operate on the file system at the same time, so the file-system code must coordinate to maintain invariants.

- Accessing a disk is orders of magnitude slower than accessing memory, so the file system must maintain an in-memory cache of popular blocks.

- The file system must support *crash recovery*. That is, if a crash (e.g., power failure) occurs, the file system must still work correctly after a restart. The risk is that a crash might interrupt a sequence of updates and leave inconsistent on-disk data structures (e.g., a block that is both used in a file and marked free).

The rest of this chapter explains how xv6 addresses the first three challenges, while Chapter [11](/book/chapter-11/#CH:LOG) focuses on crash recovery.

## Overview

The xv6 file system implementation is organized in seven layers, shown in Figure [10.1](#fig:fslayer). The disk layer reads and writes blocks on a virtio hard drive; virtio is an emulated disk provided by `qemu`. The buffer cache layer caches disk blocks and synchronizes access to them, making sure that only one kernel process at a time can modify the data stored in any particular block. The logging layer allows higher layers to wrap updates to several blocks in a *transaction*, and ensures that the blocks are updated atomically in the face of crashes (i.e., all of them are updated or none). The inode layer provides individual files, each represented as an *inode* with a unique i-number and some blocks holding the file's data. The directory layer implements each directory as a special kind of inode whose content is a sequence of directory entries, each of which contains a file's name and i-number. The pathname layer provides hierarchical path names like `/usr/rtm/xv6/fs.c`, and resolves them with recursive lookup. The file descriptor layer abstracts many Unix resources (e.g., pipes, devices, files, etc.) using the file system interface, simplifying the lives of application programmers.

<figure id="fig:fslayer" data-latex-placement="t">
<img src="/book/fig/fslayer.svg" alt="" loading="lazy">
<figcaption>
Figure 10.1: Layers of the xv6 file system.
</figcaption>
</figure>

Disk hardware traditionally presents the data on the disk as a numbered sequence of 512-byte *sectors*: sector 0 is the first 512 bytes, sector 1 is the next, and so on. The disk hardware supports reads and writes only in whole multiples of sectors. Thus, for example, if the operating system needs to change a single byte on the disk, it must read the whole surrounding sector into memory, update the one byte, and then write the whole sector back to the disk. For this reason, file systems allocate disk space for files in units of one or more sectors. The allocation granularity that a file system uses is called the block size. Most file systems use multi-sector blocks for better efficiency; xv6 uses a block size of two sectors (determined by `BSIZE` in `fs.h`).

Xv6 holds copies of blocks that it has read into memory in objects of type [`struct buf`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/buf.h#L1). The data stored in this structure is sometimes not the same as the data on the disk: it might have not yet been read in from disk (the disk is working on it but hasn't returned the block's content yet), or it might have been updated by software but not yet written to the disk.

<figure id="fig:fslayout" data-latex-placement="t">
<img src="/book/fig/fslayout.svg" alt="" loading="lazy">
<figcaption>
Figure 10.2: Structure of the xv6 file system.
</figcaption>
</figure>

The file system must have a plan for where it stores inodes and content blocks on the disk. To do so, xv6 divides the disk into several sections, as Figure [10.2](#fig:fslayout) shows. The file system does not use block 0 (block 0 often holds boot code, though in xv6 it's merely unused). Block 1 is called the *superblock*; it contains metadata about the file system (the file system size in blocks, the number of data blocks, the number of inodes, and the number of blocks in the log). Blocks starting at 2 hold the log. After the log are the inodes, with multiple inodes per block. After those come bitmap blocks tracking which data blocks are in use. The remaining blocks are data blocks; each is either marked free in the bitmap block, or holds content for a file or directory.

An xv6 file system is initially created by a program outside of xv6 called `mkfs`. `mkfs` writes the superblock, initializes an inode for the (empty) root directory, and marks all i-nodes and data blocks as free.

At this point please read the files [`kernel/buf.h`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/buf.h), [`kernel/fs.h`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h), [`kernel/fs.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c), [`kernel/sysfile.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c), and [`kernel/file.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c).

<a id="s:bcache"></a>

## Buffer cache layer
The buffer cache has two jobs: (1) synchronize access to disk blocks to ensure that only one copy of a block is in memory and that only one kernel thread at a time uses that copy; (2) cache popular blocks so that they don't need to be re-read from the slow disk. The code is in `bio.c`.

The main interface exported by the buffer cache consists of `bread` and `bwrite`; the former obtains a *buf* containing a copy of a block which can be read or modified in memory, and the latter writes a modified buffer to the appropriate block on the disk. A kernel thread must release a buffer by calling `brelse` when it is done with it. The buffer cache uses a per-buffer sleep-lock to ensure that only one thread at a time uses each buffer (and thus each disk block); `bread` returns a locked buffer, and `brelse` releases the lock.

The buffer cache has a fixed number of buffers to hold disk blocks, which means that if the file system asks for a block that is not already in the cache, the buffer cache must recycle a buffer currently holding some other block. The buffer cache recycles the least recently used buffer for the new block. The assumption is that the least recently used buffer is the one least likely to be used again soon.

There are two interactions between the buffer cache layer and the logging layer: 1) to implement transactions, file-system layers above the logging layer update a disk block by calling [`log_write`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/log.c#L224), which is a proxy for `bwrite` and only the logging layer calls `bwrite`; 2) the buffer cache cannot recycle a buffer that is in use by a transaction until the logging layer commits the transaction (see Section [11.2](/book/chapter-11/#s:code-logging)). To avoid recycling too early, the logging layer uses the buffer-cache functions `bpin` and `bunpin` to pin and unpin a buffer in the buffer cache.

At this point please read the files [`kernel/bio.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c) and [`kernel/log.c`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/log.c).

## Code: Buffer cache

The buffer cache is a doubly-linked list of buffers. The function `binit`, called by [`main`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/main.c#L27), initializes the list with the `NBUF` buffers in the static array [`buf`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L42-L51). All other access to the buffer cache refer to the linked list via `bcache.head`, not the `buf` array.

A buffer has two state fields associated with it. The field `valid` indicates that the buffer contains a copy of the block. The field `disk` indicates that the buffer content has been handed to the disk, which may change the buffer (e.g., write data from the disk into `data`).

[`bread`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L92) calls `bget` to get a buffer for the given block [(bio.c:96)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L96). If the buffer needs to be read from disk, `bread` calls `virtio_disk_rw` to do that before returning the buffer.

[`bget`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L58) scans the buffer list for a buffer with the given device and block numbers [(bio.c:64-72)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L64-L72). If there is such a buffer, `bget` acquires the sleep-lock for the buffer. `bget` then returns the locked buffer.

If there is no cached buffer for the given block, `bget` must make one, possibly reusing a buffer that held a different block. It scans the buffer list a second time, looking for a buffer that is not in use (`b->refcnt = 0`); any such buffer can be used. `bget` edits the buffer metadata to record the new device and block number and acquires its sleep-lock. Note that the assignment `b->valid = 0` ensures that `bread` will read the block data from disk rather than incorrectly using the buffer's previous contents.

It is important that there is at most one cached buffer per disk block, to ensure that readers see writes, and because the file system uses locks on buffers for synchronization. `bget` ensures this invariant by holding the `bcache.lock` continuously from the first loop's check of whether the block is cached through the second loop's declaration that the block is now cached (by setting `dev`, `blockno`, and `refcnt`). This causes the check for a block's presence and (if not present) the designation of a buffer to hold the block to be atomic.

It is safe for `bget` to acquire the buffer's sleep-lock outside of the `bcache.lock` critical section, since the non-zero `b->refcnt` prevents the buffer from being re-used for a different disk block. The sleep-lock protects reads and writes of the block's buffered content, while the `bcache.lock` protects information about which blocks are cached.

If all the buffers are busy, then too many processes are simultaneously executing file system calls; `bget` panics. A more graceful response might be to sleep until a buffer became free, though there would then be a possibility of deadlock.

Once `bread` has read the disk (if needed) and returned the buffer to its caller, the caller has exclusive use of the buffer and can read or write the data bytes. If the caller does modify the buffer, it must call `bwrite` to write the changed data to disk before releasing the buffer. [`bwrite`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L107) calls `virtio_disk_rw` to talk to the disk hardware.

When the caller is done with a buffer, it must call `brelse` to release it. (The name `brelse`, a shortening of b-release, is cryptic but worth learning: it originated in Unix and is used in BSD, Linux, and Solaris too.) [`brelse`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L117) releases the sleep-lock and moves the buffer to the front of the linked list [(bio.c:128-133)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/bio.c#L128-L133). Moving the buffer causes the list to be ordered by how recently the buffers were used (meaning released): the first buffer in the list is the most recently used, and the last is the least recently used. The two loops in `bget` take advantage of this: the scan for an existing buffer must process the entire list in the worst case, but checking the most recently used buffers first (starting at `bcache.head` and following `next` pointers) will reduce scan time when there is good locality of reference. The scan to pick a buffer to reuse picks the least recently used buffer by scanning backward (following `prev` pointers).

## Code: Block allocator

File and directory content is stored in disk blocks, which must be allocated from a free pool. Xv6's block allocator maintains a free bitmap on disk, with one bit per block. A zero bit indicates that the corresponding block is free; a one bit indicates that it is in use. When it creates a new file system, `mkfs` sets the bits corresponding to the boot sector, superblock, log blocks, inode blocks, and bitmap blocks.

The block allocator provides two functions: `balloc` allocates a new disk block, and `bfree` frees a block. The loop in `balloc` at [(fs.c:74)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L74) considers every block, starting at block 0 up to `sb.size`, the number of blocks in the file system. It looks for a block whose bitmap bit is zero, indicating that it is free. If `balloc` finds such a block, it updates the bitmap and returns the block. For efficiency, the loop is split into two pieces. The outer loop reads each block of bitmap bits. The inner loop checks all Bits-Per-Block (`BPB`) bits in a single bitmap block. The race that might occur if two processes try to allocate a block at the same time is prevented by the fact that the buffer cache only lets one process use any one bitmap block at a time (see Section [10.2](/book/chapter-10/#s:bcache)).

[`bfree`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L94) finds the right bitmap block and clears the right bit. Again the exclusive use implied by `bread` and `brelse` avoids the need for explicit locking.

As with much of the code described in the remainder of this chapter, `balloc` and `bfree` must be called inside a transaction.

## Inode layer

The term *inode* can have one of two related meanings. It might refer to the on-disk data structure containing a file's size and list of data block numbers. Or "inode" might refer to an in-memory inode, which contains a copy of the on-disk inode as well as extra information needed within the kernel.

The on-disk inodes are packed into a contiguous area of disk called the inode blocks. Every inode is the same size, so it is easy, given a number n, to find the nth inode on the disk. In fact, this number n, called the inode number or i-number, is how inodes are identified in the implementation.

The on-disk inode is defined by a [`struct dinode`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h#L32). The `type` field distinguishes between files, directories, and special files (devices). A type of zero indicates that an on-disk inode is free. The `nlink` field counts the number of directory entries that refer to this inode, in order to recognize when the on-disk inode and its data blocks should be freed. The `size` field records the number of bytes of content in the file. The `addrs` array records the block numbers of the disk blocks holding the file's content.

The kernel keeps the set of active inodes in memory in a table called `itable`; [`struct inode`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.h#L17) is the in-memory copy of a `struct` `dinode` on disk. The kernel stores an inode in memory only if there are C pointers referring to that inode. The `ref` field counts the number of C pointers referring to the in-memory inode, and the kernel discards the inode from memory if the reference count drops to zero. The `iget` and `iput` functions acquire and release pointers to an inode, modifying the reference count. Pointers to an inode can come from file descriptors, current working directories, and transient kernel code such as `kexec`.

There are four lock or lock-like mechanisms in xv6's inode code. `itable.lock` protects the invariant that an inode is present in the inode table at most once, and the invariant that an in-memory inode's `ref` field counts the number of in-memory pointers to the inode. Each in-memory inode has a `lock` field containing a sleep-lock, which ensures exclusive access to the inode's fields (such as file length) as well as to the inode's file or directory content blocks. An inode's `ref`, if it is greater than zero, causes the system to maintain the inode in the table, and not re-use the table entry for a different inode. Finally, each inode contains a `nlink` field (on disk and copied in memory if in memory) that counts the number of directory entries that refer to a file; xv6 won't free an inode if its link count is greater than zero.

A `struct` `inode` pointer returned by `iget()` is guaranteed to be valid until the corresponding call to `iput()`; the inode won't be deleted, and the memory referred to by the pointer won't be re-used for a different inode. `iget()` provides non-exclusive access to an inode, so that there can be many pointers to the same inode. Many parts of the file-system code depend on this behavior of `iget()`, both to hold long-term references to inodes (as open files and current directories) and to prevent races while avoiding deadlock in code that manipulates multiple inodes (such as pathname lookup).

The `struct` `inode` that `iget` returns may not have any useful content. In order to ensure it holds a copy of the on-disk inode, code must call `ilock`. This locks the inode (so that no other process can `ilock` it) and reads the inode from the disk, if it has not already been read. `iunlock` releases the lock on the inode. Separating acquisition of inode pointers from locking helps avoid deadlock in some situations, for example during directory lookup. Multiple processes can hold a C pointer to an inode returned by `iget`, but only one process can lock the inode at a time.

The inode table only stores inodes to which kernel code or data structures hold C pointers. Its main job is synchronizing access by multiple processes. The inode table also happens to cache frequently-used inodes, but caching is secondary; if an inode is used frequently, the buffer cache will probably keep it in memory. Code that modifies an in-memory inode writes it to disk with `iupdate`.

## Code: Inodes

To allocate a new inode (for example, when creating a file), xv6 calls [`ialloc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L201). `ialloc` is similar to `balloc`: it loops over the inode structures on the disk, one block at a time, looking for one that is marked free. When it finds one, it claims it by writing the new `type` to the disk and then returns an entry from the inode table with the tail call to [`iget`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L215). The correct operation of `ialloc` depends on the fact that only one process at a time can be holding a reference to `bp`: `ialloc` can be sure that some other process does not simultaneously see that the inode is available and try to claim it.

[`iget`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L249) looks through the inode table for an active entry (`ip->ref` `>` `0`) with the desired device and inode number. If it finds one, it returns a new reference to that inode [(fs.c:258-262)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L258-L262). As `iget` scans, it records the position of the first empty slot [(fs.c:263-264)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L263-L264), which it uses if it needs to allocate a table entry.

Code must lock the inode using `ilock` before reading or writing its metadata or content. [`ilock`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L295) uses a sleep-lock for this purpose. Once `ilock` has exclusive access to the inode, it reads the inode from disk (more likely, the buffer cache) if needed. The function [`iunlock`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L323) releases the sleep-lock, which may cause any processes sleeping to be woken up.

[`iput`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L350) releases a C pointer to an inode by decrementing the reference count [(fs.c:373)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L373). If this is the last reference, the inode's slot in the inode table is now free and can be re-used for a different inode.

If `iput` sees that there are no C pointer references to an inode and that the inode has no links to it (occurs in no directory) [(fs.c:357)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L357), then the inode and its data blocks must be freed. `iput` calls `itrunc` to truncate the file to zero bytes, freeing the data blocks, and then calls [`ifree`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L333) to set the inode's type to 0 (unallocated) on disk.

The locking protocol in `iput` in the case in which it frees the inode deserves a closer look. One danger is that a concurrent thread might be waiting in `ilock` to use this inode (e.g., to read a file or list a directory), and won't be prepared to find that the inode is no longer allocated. This can't happen because there is no way for a system call to get a pointer to an in-memory inode if it has no links to it and `ip->ref` is one. That one reference is the reference owned by the thread calling `iput`. The other main danger is that a concurrent call to `ialloc` might choose the same inode that `iput` is freeing. This can happen only after `ifree` writes the disk so that the inode has type zero. `iput` calls `ifree` last, after it has truncated the inode, released the inode's sleep-lock, and dropped its own reference. So by the time the allocating thread can see that the inode is free, `iput` is done with it, and the race is benign.

`iput()` can write to the disk. This means that any system call that uses the file system may write to the disk, because the system call may be the last one having a reference to the file. Even calls like `read()` that appear to be read-only, may end up calling `iput().` This, in turn, means that even read-only system calls must be wrapped in transactions if they use the file system.

There is a challenging interaction between `iput()` and crashes. `iput()` doesn't truncate a file immediately when the link count for the file drops to zero, because some process might still hold a reference to the inode in memory: a process might still be reading and writing to the file, because it successfully opened it. But, if a crash happens before the last process closes the file descriptor for the file, then the file will be marked allocated on disk but no directory entry will point to it.

File systems handle this case in one of two ways. The simple solution is that on recovery, after reboot, the file system scans the whole file system for files that are marked allocated, but have no directory entry pointing to them. If any such file exists, then it can free those files. This is the solution implemented by xv6. During boot, `fsinit` calls [`ireclaim`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L389), which examines every on-disk inode, looking for ones that are marked allocated (non-zero type) but have a link count of zero; these are exactly the files that had been unlinked but were still open when the crash occurred. For each such orphan, `ireclaim` calls `iput` to free the inode and its data blocks.

The second solution doesn't require scanning the file system. In this solution, the file system records on disk (e.g., in the super block) the inode inumber of a file whose link count drops to zero but whose reference count isn't zero. If the file system removes the file when its reference count reaches 0, then it updates the on-disk list by removing that inode from the list. On recovery, the file system frees any file in the list.

## Code: Inode content

<figure id="fig:inode" data-latex-placement="t">
<img src="/book/fig/inode.svg" alt="" loading="lazy">
<figcaption>
Figure 10.3: The representation of a file on disk.
</figcaption>
</figure>

The on-disk inode structure, `struct dinode`, contains a size and an array of block numbers (see Figure [10.3](#fig:inode)). The inode data is found in the blocks listed in the `dinode` 's `addrs` array. The first `NDIRECT` blocks of data are listed in the first `NDIRECT` entries in the array; these blocks are called *direct blocks*. The next `NINDIRECT` blocks of data are listed not in the inode but in a data block called the *indirect block*. The last entry in the `addrs` array gives the address of the indirect block. Thus the first 12 kB ( `NDIRECT` `x` `BSIZE`) bytes of a file can be loaded from blocks listed in the inode, while the next `256` kB ( `NINDIRECT` `x` `BSIZE`) bytes can only be loaded after consulting the indirect block. This is a good on-disk representation but a complex one for clients. The function `bmap` manages the representation so that higher-level routines, such as `readi` and `writei`, which we will see shortly, do not need to manage this complexity. `bmap` returns the disk block number of the `bn`'th data block for the inode `ip`. If `ip` does not have such a block yet, `bmap` allocates one.

The function [`bmap`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L421) begins by picking off the easy case: the first `NDIRECT` blocks are listed in the inode itself [(fs.c:426-434)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L426-L434). The next `NINDIRECT` blocks are listed in the indirect block at `ip->addrs[NDIRECT]`. `bmap` reads the indirect block [(fs.c:445)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L445) and then reads a block number from the right position within the block [(fs.c:446)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L446). If the block number exceeds `NDIRECT+NINDIRECT`, `bmap` panics; `writei` contains the check that prevents this from happening [(fs.c:551)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L551).

`bmap` allocates blocks as needed. An `ip->addrs[]` or indirect entry of zero indicates that no block is allocated. As `bmap` encounters zeros, it replaces them with the numbers of fresh blocks, allocated on demand [(fs.c:427-428)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L427-L428) [(fs.c:439-440)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L439-L440).

`itrunc` frees a file's blocks, resetting the inode's size to zero. [`itrunc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L464) starts by freeing the direct blocks [(fs.c:470-475)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L470-L475), then the ones listed in the indirect block [(fs.c:480-483)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L480-L483), and finally the indirect block itself [(fs.c:485-486)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L485-L486).

`bmap` makes it easy for `readi` and `writei` to get at an inode's data. [`readi`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L510) starts by making sure that the offset and count are not beyond the end of the file. Reads that start beyond the end of the file return an error [(fs.c:515-516)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L515-L516) while reads that start at or cross the end of the file return fewer bytes than requested [(fs.c:517-518)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L517-L518). The main loop processes each block of the file, copying data from the buffer into [`dst`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L520-L532). [`writei`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L544) is identical to `readi`, with three exceptions: writes that start at or cross the end of the file grow the file, up to the maximum file size [(fs.c:551-552)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L551-L552); the loop copies data into the buffers instead of out [(fs.c:560)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L560); and if the write has extended the file, `writei` must update its size [(fs.c:570-571)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L570-L571).

The function [`stati`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L496) copies inode metadata into the `stat` structure, which is exposed to user programs via the `stat` system call.

## Code: Directory layer

A directory is implemented internally much like a file. Its inode has type `T_DIR` and its data is a sequence of directory entries. Each entry is a [`struct dirent`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h#L58), which contains a name and an inode number. The name is at most `DIRSIZ` (14) characters; if shorter, it is terminated by a NULL (0) byte. Directory entries with inode number zero are free.

The reason why a directory entry contains the file's i-number rather than the named file's entire i-node is to support "links" created by the `link()` system call. An i-node can be referred to in multiple directories, and thus under multiple path names; each of an i-node's names (i.e. directory entries) is called a link. Because of the possibility of an i-node being named in multiple directories, it is not convenient for the i-node to be stored in any one of those directories. The possibility of multiple links is also the reason for the `nlink` field in the i-node. The fact that i-nodes are stored separately from directories also allows sensible handling of a file being unlinked (removed) while some process has a file descriptor referring to the file: the file descriptor refers to the i-node, not to any directory entry, so the file descriptor will still work even though the file (really, just its name) has been removed.

The function [`dirlookup`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L592) searches a directory for an entry with the given name. If it finds one, it returns a pointer to the corresponding inode, unlocked, and sets `*poff` to the byte offset of the entry within the directory, in case the caller wishes to edit it. If `dirlookup` finds an entry with the right name, it updates `*poff` and returns an unlocked inode obtained via `iget`. `dirlookup` is the reason that `iget` returns unlocked inodes. The caller has locked `dp`, so if the lookup was for `.`, an alias for the current directory, attempting to lock the inode before returning would try to re-lock `dp` and deadlock. (There are more complicated deadlock scenarios involving multiple processes and `..`, an alias for the parent directory; `.` is not the only problem.) The caller can unlock `dp` and then lock `ip`, ensuring that it only holds one lock at a time.

The function [`dirlink`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L620) writes a new directory entry with the given name and inode number into the directory `dp`. If the name already exists, `dirlink` returns an error [(fs.c:626-630)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L626-L630). The main loop reads directory entries looking for an unallocated entry. When it finds one, it stops the loop early [(fs.c:632-637)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L632-L637), with `off` set to the offset of the available entry. Otherwise, the loop ends with `off` set to `dp->size`. Either way, `dirlink` then adds a new entry to the directory by writing at offset [`off`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L642-L643).

## Code: Path names

Path name lookup involves a succession of calls to `dirlookup`, one for each path component. [`namei`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L731) evaluates `path` and returns the corresponding `inode`. The function `nameiparent` is a variant: it stops before the last element, returning the inode of the parent directory and copying the final element into `name`. Both call the generalized function `namex` to do the real work.

[`namex`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L692) starts by deciding where the path evaluation begins. If the path begins with a slash, evaluation begins at the root; otherwise, the current directory [(fs.c:696-699)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L696-L699). Then it uses `skipelem` to consider each element of the path in turn [(fs.c:701)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L701). Each iteration of the loop must look up `name` in the current inode `ip`. The iteration begins by locking `ip` and checking that it is a directory. If not, the lookup fails [(fs.c:702-706)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L702-L706). (Locking `ip` is necessary not because `ip->type` can change underfoot---it can't---but because until `ilock` runs, `ip->type` is not guaranteed to have been loaded from disk.) If the call is `nameiparent` and this is the last path element, the loop stops early, as per the definition of `nameiparent`; the final path element has already been copied into `name`, so `namex` need only return the unlocked [`ip`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L711-L715). Finally, the loop looks for the path element using `dirlookup` and prepares for the next iteration by setting [`ip = next`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c#L716-L721). When the loop runs out of path elements, it returns `ip`.

The procedure `namex` may take a long time to complete: it could involve several disk operations to read inodes and directory blocks for the directories traversed in the pathname (if they are not in the buffer cache). Xv6 is carefully designed so that if an invocation of `namex` by one kernel thread is blocked on a disk I/O, another kernel thread looking up a different pathname can proceed concurrently. `namex` locks each directory in the path separately so that lookups in different directories can proceed in parallel.

This concurrency introduces some challenges. For example, while one kernel thread is looking up a pathname another kernel thread may be changing the directory tree by unlinking a directory. A potential risk is that a lookup may be searching a directory that has been deleted by another kernel thread and its blocks have been re-used for another directory or file.

Xv6 avoids such races. For example, when executing `dirlookup` in `namex`, the lookup thread holds the lock on the directory and `dirlookup` returns an inode that was obtained using `iget`. `iget` increases the reference count of the inode. Only after receiving the inode from `dirlookup` does `namex` release the lock on the directory. Now another thread may unlink the inode from the directory but xv6 will not delete the inode yet, because the reference count of the inode is still larger than zero.

Another risk is deadlock. For example, `next` points to the same inode as `ip` when looking up \".\". Locking `next` before releasing the lock on `ip` would result in a deadlock. To avoid this deadlock, `namex` unlocks the directory before obtaining a lock on `next`. Here again we see why the separation between `iget` and `ilock` is important.

## File descriptor layer

A cool aspect of the Unix interface is that most resources in Unix are represented as files, including devices such as the console, pipes, and of course, real files. The file descriptor layer is the layer that achieves this uniformity.

Xv6 gives each process its own table of open files, or file descriptors, as we saw in Chapter [1](/book/chapter-1/#CH:UNIX). Each open file is represented by a [`struct file`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.h#L1), which is a wrapper around either an inode or a pipe, plus an I/O offset. Each call to `open` creates a new open file (a new `struct` `file`): if multiple processes open the same file independently, the different instances will have different I/O offsets. On the other hand, a single open file (the same `struct` `file`) can appear multiple times in one process's file table and also in the file tables of multiple processes. This would happen if one process used `open` to open the file and then created aliases using `dup` or shared it with a child using `fork`. A reference count tracks the number of references to a particular open file. A file can be open for reading or writing or both. The `readable` and `writable` fields track this.

All the open files in the system are kept in a global file table, the `ftable`. The file table has functions to allocate a file (`filealloc`), create a duplicate reference (`filedup`), release a reference (`fileclose`), and read and write data (`fileread` and `filewrite`).

The first three follow the now-familiar form. [`filealloc`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L30) scans the file table for an unreferenced file (`f->ref` `==` `0`) and returns a new reference; [`filedup`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L48) increments the reference count; and [`fileclose`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L60) decrements it. When a file's reference count reaches zero, `fileclose` releases the underlying pipe or inode, according to the type.

The functions `filestat`, `fileread`, and `filewrite` implement the `fstat`, `read`, and `write` operations on files. [`filestat`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L88) is only allowed on inodes and calls `stati`. `fileread` and `filewrite` check that the operation is allowed by the open mode and then pass the call through to either the pipe or inode implementation. If the file represents an inode, `fileread` and `filewrite` use the I/O offset as the offset for the operation and then advance it [(file.c:122-123)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L122-L123) [(file.c:162-163)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L162-L163). Pipes have no concept of offset. Recall that the inode functions require the caller to handle locking [(file.c:94-96)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L94-L96) [(file.c:121-124)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L121-L124) [(file.c:161-164)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/file.c#L161-L164). The inode locking has the convenient side effect that the read and write offsets are updated atomically, so that multiple writing to the same file simultaneously cannot overwrite each other's data, though their writes may end up interlaced.

## Code: System calls

With the functions that the lower layers provide, the implementation of most system calls is trivial (see [(sysfile.c)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c)). There are a few calls that deserve a closer look.

The functions `sys_link` and `sys_unlink` edit directories, creating or removing references to inodes. They are another good example of the power of using transactions. [`sys_link`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L124) begins by fetching its arguments, two strings `old` and [`new`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L129). Assuming `old` exists, is not a directory, and has fewer than `NLINK_MAX` links [(sysfile.c:133-145)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L133-L145), `sys_link` increments its `ip->nlink` count [(sysfile.c:151)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L151). The limit exists because `nlink` is a 16-bit field in the on-disk inode; without the check, creating too many links would overflow the count. Then `sys_link` calls `nameiparent` to find the parent directory and final path element of [`new`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L155) and creates a new directory entry pointing at `old` 's inode [(sysfile.c:165)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L165). The new parent directory must exist and be on the same device as the existing inode: inode numbers only have a unique meaning on a single disk. If an error like this occurs, `sys_link` must go back and decrement `ip->nlink`.

Transactions simplify the implementation because it requires updating multiple disk blocks, but we don't have to worry about the order in which we do them. They either will all succeed or none. For example, without transactions, updating `ip->nlink` before creating a link, would put the file system temporarily in an unsafe state, and a crash in between could result in havoc. With transactions we don't have to worry about this.

nally, now that the data is initialized properly, `create` can link it into the parent directory [(sysfile.c:306)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L306). `create`, like `sys_link`, holds two inode locks simultaneously: `ip` and `dp`. There is no possibility of deadlock because the inode `ip` is freshly allocated: no other process in the system will hold `ip` 's lock and then try to lock `dp`.

Using `create`, it is easy to implement `sys_open`, `sys_mkdir`, and `sys_mknod`. [`sys_open`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L329) is the most complex, because creating a new file is only a small part of what it can do. If `open` is passed the `O_CREATE` flag, it calls [`create`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L344). Otherwise, it calls [`namei`](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L350). `create` returns a locked inode, but `namei` does not, so `sys_open` must lock the inode itself. This provides a convenient place to check that directories are only opened for reading, not writing. Assuming the inode was obtained one way or the other, `sys_open` allocates a file and a file descriptor [(sysfile.c:368)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L368) and then fills in the file [(sysfile.c:380-385)](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sysfile.c#L380-L385). Note that no other process can access the partially initialized file since it is only in the current process's table.

Chapter [9](/book/chapter-9/#CH:SLEEP) examined the implementation of pipes before we even had a file system. The function `sys_pipe` connects that implementation to the file system by providing a way to create a pipe pair. Its argument is a pointer to space for two integers, where it will record the two new file descriptors. Then it allocates the pipe and installs the file descriptors.

## Real world

The buffer cache in a real-world operating system is significantly more complex than xv6's, but it serves the same two purposes: caching and synchronizing access to the disk. Xv6's buffer cache uses a least recently used (LRU) eviction policy; there are many more complex policies that can be implemented, each good for some workloads and not as good for others. A more efficient LRU cache would eliminate the linked list, instead using a hash table for lookups and a heap for LRU evictions. Modern buffer caches are typically integrated with the virtual memory system to support memory-mapped files.

Xv6 uses an on-disk layout of inodes and directories similar to that of early UNIX; this scheme has been remarkably persistent over the years. BSD's UFS/FFS and Linux's ext2/ext3 use essentially the same data structures. The most inefficient part of the file system layout is the directory, which requires a linear scan over all the disk blocks during each lookup. This is reasonable when directories are only a few disk blocks, but is expensive for directories holding many files. Microsoft Windows's NTFS, macOS's HFS, and Solaris's ZFS, just to name a few, implement a directory as an on-disk balanced tree of blocks. This is complicated but guarantees logarithmic-time directory lookups.

Xv6 requires that the file system fit on one disk device and not change in size. As large databases and multimedia files drive storage requirements ever higher, operating systems are developing ways to eliminate the "one disk per file system" bottleneck. The basic approach is to combine many disks into a single logical disk. Hardware solutions such as RAID are still the most popular, but the current trend is moving toward implementing as much of this logic in software as possible. These software implementations typically allow rich functionality like growing or shrinking the logical device by adding or removing disks on the fly. Of course, a storage layer that can grow or shrink on the fly requires a file system that can do the same: the fixed-size array of inode blocks used by xv6 would not work well in such environments. Separating disk management from the file system may be the cleanest design, but the complex interface between the two has led some systems, like Sun's ZFS, to combine them.

Xv6's file system lacks many other features of modern file systems; for example, it lacks support for snapshots and incremental backup.

Modern Unix systems allow many kinds of resources to be accessed with the same system calls as on-disk storage: named pipes, network connections, remotely-accessed network file systems, and monitoring and control interfaces such as `/proc`. Instead of xv6's `if` statements in `fileread` and `filewrite`, these systems typically give each open file a table of function pointers, one per operation, and call the function pointer to invoke that inode's implementation of the call. Network file systems and user-level file systems provide functions that turn those calls into network RPCs and wait for the response before returning.

## Exercises

1.  Why panic in `balloc` ? Can xv6 recover?

2.  Why panic in `ialloc` ? Can xv6 recover?

3.  Why doesn't `filealloc` panic when it runs out of files? Why is this more common and therefore worth handling?

4.  Suppose the file corresponding to `ip` gets unlinked by another process between `sys_link` 's calls to `iunlock(ip)` and `dirlink`. Will the link be created correctly? Why or why not?

5.  `create` makes four function calls (one to `ialloc` and three to `dirlink`) that it requires to succeed. If any doesn't, `create` calls `panic`. Why is this acceptable? Why can't any of those four calls fail?

6.  `sys_chdir` calls `iunlock(ip)` before `iput(cp->cwd)`, which might try to lock `cp->cwd`, yet postponing `iunlock(ip)` until after the `iput` would not cause deadlocks. Why not?

7.  Implement the `lseek` system call. Supporting `lseek` will also require that you modify `filewrite` to fill holes in the file with zero if `lseek` sets `off` beyond `f->ip->size.`

8.  Add `O_TRUNC` and `O_APPEND` to `open`, so that `>` and `>>` operators work in the shell.

9.  Modify the file system to support symbolic links.

10. Modify the file system to support named pipes.

11. Modify the file and VM system to support memory-mapped files.
