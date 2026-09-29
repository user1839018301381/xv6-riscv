// clang-format off
struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;

// bio.c（バッファ入出力・ブロックキャッシュ）
void            binit(void);
struct buf*     bread(uint device, uint block_number);
void            brelse(struct buf *buffer);
void            bwrite(struct buf *buffer);
void            bpin(struct buf *buffer);
void            bunpin(struct buf *buffer);

// console.c（コンソール入出力）
void            consoleinit(void);
void            consoleintr(int character);
void            consputc(int character);

// exec.c（プログラムの実行）
int             kexec(char *path, char **argv);

// file.c（ファイル記述子の操作）
struct file*    filealloc(void);
void            fileclose(struct file *file);
struct file*    filedup(struct file *file);
void            fileinit(void);
int             fileread(struct file *file, uint64 destination_address, int byte_count);
int             filestat(struct file *file, uint64 status_address);
int             filewrite(struct file *file, uint64 source_address, int byte_count);

// fs.c（ファイルシステム）
void            fsinit(int device);
int             dirlink(struct inode *directory_inode, char *name,
                        uint inode_number);
struct inode*   dirlookup(struct inode *directory_inode, char *name,
                          uint *offset_out);
struct inode*   ialloc(uint device, short inode_type);
struct inode*   idup(struct inode *inode);
void            iinit(void);
void            ilock(struct inode *inode);
void            iput(struct inode *inode);
void            iunlock(struct inode *inode);
void            iunlockput(struct inode *inode);
void            iupdate(struct inode *inode);
int             namecmp(const char *left, const char *right);
struct inode*   namei(char *path);
struct inode*   nameiparent(char *path, char *name);
int             readi(struct inode *inode, int destination_is_user,
                      uint64 destination_address, uint offset, uint byte_count);
void            stati(struct inode *inode, struct stat *file_status);
int             writei(struct inode *inode, int source_is_user,
                       uint64 source_address, uint offset, uint byte_count);
void            itrunc(struct inode *inode);
void            ireclaim(int device);

// kalloc.c（物理ページ割当て）
void*           kalloc(void);
void            kfree(void *page_address);
void            kinit(void);

// log.c（ファイルシステムログ）
void            initlog(int device, struct superblock *superblock);
void            log_write(struct buf *buffer);
void            begin_op(void);
void            end_op(void);

// pipe.c（パイプ）
int             pipealloc(struct file **read_file_out, struct file **write_file_out);
void            pipeclose(struct pipe *pipe, int is_writable);
int             piperead(struct pipe *pipe, uint64 destination_address, int byte_count);
int             pipewrite(struct pipe *pipe, uint64 source_address, int byte_count);

// printk.c（カーネル用表示・異常終了処理）
int             printk(char *format, ...) __attribute__ ((format (printf, 1, 2)));
void            panic(char *message) __attribute__((noreturn));
void            printkinit(void);

// proc.c（プロセス管理）
int             cpuid(void);
void            kexit(int exit_status);
int             kfork(void);
int             growproc(int size_delta);
void            proc_mapstacks(pagetable_t kernel_pagetable);
pagetable_t     proc_pagetable(struct proc *process);
void            proc_freepagetable(pagetable_t pagetable, uint64 memory_size);
int             kkill(int pid);
int             is_killed(struct proc *process);
void            mark_killed(struct proc *process);
struct cpu*     mycpu(void);
struct proc*    myproc(void);
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            sleep_prepare(void *channel);
void            sleep(void);
void            userinit(void);
int             kwait(uint64 status_address);
void            wakeup(void *channel);
void            yield(void);
int             either_copyout(int destination_is_user, uint64 destination_address,
                               void *source, uint64 byte_count);
int             either_copyin(void *destination, int source_is_user,
                              uint64 source_address, uint64 byte_count);
void            procdump(void);

// swtch.S（コンテキスト切替えのアセンブリ）
void            swtch(struct context *old_context,
                      struct context *new_context);

// spinlock.c（スピンロック）
void            acquire(struct spinlock *lock);
int             holding(struct spinlock *lock);
void            initlock(struct spinlock *lock, char *name);
void            release(struct spinlock *lock);
void            push_off(void);
void            pop_off(void);

// sleeplock.c（スリープロック）
void            acquiresleep(struct sleeplock *lock);
void            releasesleep(struct sleeplock *lock);
int             holdingsleep(struct sleeplock *lock);
void            initsleeplock(struct sleeplock *lock, char *name);

// string.c（文字列・メモリ操作）
int             memcmp(const void *left, const void *right, uint byte_count);
void*           memmove(void *destination, const void *source, uint byte_count);
void*           memset(void *destination, int value, uint byte_count);
char*           safestrcpy(char *destination, const char *source, int destination_size);
int             strlen(const char *string);
int             strncmp(const char *left, const char *right, uint max_length);
char*           strncpy(char *destination, const char *source, int max_length);

// syscall.c（システムコール処理）
void            argint(int argument_index, int *value_out);
int             argstr(int argument_index, char *buffer, int max_length);
void            argaddr(int argument_index, uint64 *address_out);
int             fetchstr(uint64 address, char *buffer, int max_length);
int             fetchaddr(uint64 address, uint64 *value_out);
void            syscall(void);

// trap.c（トラップ処理）
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock tickslock;
void            prepare_return(void);

// uart.c（UARTドライバ）
void            uartinit(void);
void            uartintr(void);
void            uartwrite(char buffer[], int byte_count);
void            uartputc_sync(int character);

// vm.c（仮想メモリ）
void            kvminit(void);
void            kvminithart(void);
void            kvmmap(pagetable_t pagetable, uint64 virtual_address,
                       uint64 physical_address, uint64 byte_count, int permissions);
int             mappages(pagetable_t pagetable, uint64 va, uint64 size,
                         uint64 pa, int permissions);
pagetable_t     uvmcreate(void);
uint64          uvmalloc(pagetable_t pagetable, uint64 old_size,
                         uint64 new_size, int extra_permissions);
uint64          uvmdealloc(pagetable_t pagetable, uint64 old_size,
                           uint64 new_size);
int             uvmcopy(pagetable_t parent_pagetable,
                        pagetable_t child_pagetable, uint64 memory_size);
void            uvmfree(pagetable_t pagetable, uint64 memory_size);
void            uvmunmap(pagetable_t pagetable, uint64 va, uint64 page_count,
                         int should_free_physical_pages);
void            uvmclear(pagetable_t pagetable, uint64 va);
pte_t *         walk(pagetable_t pagetable, uint64 va, int should_allocate);
uint64          walkaddr(pagetable_t pagetable, uint64 va);
int             copyout(pagetable_t pagetable, uint64 process_size,
                        uint64 destination_va, char *source, uint64 byte_count);
int             copyin(pagetable_t pagetable, uint64 process_size,
                       char *destination, uint64 source_va, uint64 byte_count);
int             copyinstr(pagetable_t pagetable, uint64 process_size,
                          char *destination, uint64 source_va,
                          uint64 max_length);
int             is_mapped(pagetable_t pagetable, uint64 va);
uint64          vmfault(pagetable_t pagetable, uint64 process_size,
                        uint64 va, int access_is_read);

// plic.c（割り込みコントローラ）
void            plicinit(void);
void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int interrupt_id);

// virtio_disk.c（virtioディスクドライバ）
void            virtio_disk_init(void);
void            virtio_disk_rw(struct buf *buffer, int is_write);
void            virtio_disk_intr(void);

// 固定長配列の要素数
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))
