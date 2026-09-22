#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc processes[NPROC];

struct proc *init_process;

int next_pid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *process);

extern char trampoline[]; // trampoline.Sのコード

// wait()中の親プロセスへの起床通知が失われないようにする。
// process->parentを使う際のメモリモデルにも従えるようにする。
// どのprocess->lockよりも先に取得しなければならない。
struct spinlock wait_lock;

// 各プロセスのカーネルスタック用にページを割り当てる。
// メモリの高位にマップし、その後ろにアクセス不能なガードページを置く。
void proc_mapstacks(pagetable_t kernel_pagetable)
{
    struct proc *process;

    for (process = processes; process < &processes[NPROC]; process++) {
        char *pa = kalloc();
        if (pa == 0)
            panic("kalloc");
        uint64 va = KSTACK((int)(process - processes));
        kvmmap(kernel_pagetable, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
}

// プロセス表を初期化する。
void procinit(void)
{
    struct proc *process;

    initlock(&pid_lock, "next_pid");
    initlock(&wait_lock, "wait_lock");
    for (process = processes; process < &processes[NPROC]; process++) {
        initlock(&process->lock, "proc");
        process->state = UNUSED;
        process->kernel_stack = KSTACK((int)(process - processes));
    }
}

// プロセスが別のCPUへ移される競合を防ぐため、
// 割り込みを無効にして呼び出さなければならない。
int cpuid()
{
    int cpu_id = r_tp();
    return cpu_id;
}

// このCPUのcpu構造体を返す。
// 割り込みは無効でなければならない。
struct cpu *mycpu(void)
{
    int cpu_id = cpuid();
    struct cpu *cpu = &cpus[cpu_id];
    return cpu;
}

// 現在のstruct proc *を返す。なければ0を返す。
struct proc *myproc(void)
{
    push_off();
    struct cpu *cpu = mycpu();
    struct proc *process = cpu->process;
    pop_off();
    return process;
}

int allocpid()
{
    int pid;

    acquire(&pid_lock);
    pid = next_pid;
    next_pid++;
    release(&pid_lock);

    return pid;
}

// プロセス表からUNUSEDのプロセスを探す。
// 見つかったらカーネルで実行するための状態を初期化し、
// process->lockを保持した状態で返す。
// 空きプロセスがないかメモリ割り当てに失敗したら0を返す。
static struct proc *allocproc(void)
{
    struct proc *process;

    for (process = processes; process < &processes[NPROC]; process++) {
        acquire(&process->lock);
        if (process->state == UNUSED) {
            goto process_found;
        } else {
            release(&process->lock);
        }
    }
    return 0;

process_found:
    process->pid = allocpid();
    process->state = USED;

    // トラップフレーム用ページを割り当てる。
    if ((process->trapframe = (struct trapframe *)kalloc()) == 0) {
        freeproc(process);
        release(&process->lock);
        return 0;
    }

    // 空のユーザページテーブル。
    process->pagetable = proc_pagetable(process);
    if (process->pagetable == 0) {
        freeproc(process);
        release(&process->lock);
        return 0;
    }

    // forkretから実行を開始する新しいコンテキストを設定する。
    // forkretはユーザ空間へ戻る。
    memset(&process->context, 0, sizeof(process->context));
    process->context.ra = (uint64)forkret;
    process->context.sp = process->kernel_stack + PGSIZE;

    return process;
}

// proc構造体と、それに属するユーザページを含むデータを解放する。
// process->lockを保持していなければならない。
static void freeproc(struct proc *process)
{
    if (process->trapframe)
        kfree((void *)process->trapframe);
    process->trapframe = 0;
    if (process->pagetable)
        proc_freepagetable(process->pagetable, process->memory_size);
    process->pagetable = 0;
    process->memory_size = 0;
    process->pid = 0;
    process->name[0] = 0;
    process->sleep_channel = 0;
    process->is_killed = 0;
    process->exit_status = 0;
    process->state = UNUSED;
}

// 指定されたプロセスのユーザページテーブルを作成する。
// ユーザメモリは持たないが、トランポリンとトラップフレームのページを持つ。
pagetable_t proc_pagetable(struct proc *process)
{
    pagetable_t pagetable;

    // 空のページテーブル。
    pagetable = uvmcreate();
    if (pagetable == 0)
        return 0;

    // トランポリンコード（システムコールから戻るため）を
    // ユーザ仮想アドレスの最高位にマップする。
    // ユーザ空間との行き来でスーパーバイザだけが使うため、PTE_Uは付けない。
    if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
                 PTE_R | PTE_X) < 0) {
        uvmfree(pagetable, 0);
        return 0;
    }

    // trampoline.S用のトラップフレームページを、
    // トランポリンページのすぐ下にマップする。
    if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(process->trapframe),
                 PTE_R | PTE_W) < 0) {
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return 0;
    }

    return pagetable;
}

// プロセスのページテーブルと、それが参照する物理メモリを解放する。
void proc_freepagetable(pagetable_t pagetable, uint64 memory_size)
{
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, memory_size);
}

// 最初のユーザプロセスを設定する。
void userinit(void)
{
    struct proc *process;

    process = allocproc();
    init_process = process;

    process->current_directory = namei("/");

    process->state = RUNNABLE;

    release(&process->lock);
}

// ユーザメモリをsize_deltaバイト増減する。
// 成功時は0、失敗時は-1を返す。
int growproc(int size_delta)
{
    uint64 memory_size;
    struct proc *process = myproc();

    memory_size = process->memory_size;
    if (size_delta > 0) {
        if (memory_size + size_delta > TRAPFRAME) {
            return -1;
        }
        if ((memory_size = uvmalloc(process->pagetable, memory_size,
                                    memory_size + size_delta, PTE_W)) == 0) {
            return -1;
        }
    } else if (size_delta < 0) {
        memory_size = uvmdealloc(process->pagetable, memory_size,
                                 memory_size + size_delta);
    }
    process->memory_size = memory_size;
    return 0;
}

// 親を複製して新しいプロセスを作成する。
// 子のカーネルスタックを、fork()システムコールから戻った状態に設定する。
int kfork(void)
{
    int i, child_pid;
    struct proc *child_process;
    struct proc *process = myproc();

    // プロセスを割り当てる。
    if ((child_process = allocproc()) == 0) {
        return -1;
    }

    // 親のユーザメモリを子へ複写する。
    if (uvmcopy(process->pagetable, child_process->pagetable, process->memory_size) < 0) {
        freeproc(child_process);
        release(&child_process->lock);
        return -1;
    }
    child_process->memory_size = process->memory_size;

    // 保存されたユーザレジスタを複写する。
    *(child_process->trapframe) = *(process->trapframe);

    // 子ではforkの戻り値が0になるようにする。
    child_process->trapframe->a0 = 0;

    // オープン中のファイルディスクリプタの参照カウントを増やす。
    for (i = 0; i < NOFILE; i++)
        if (process->open_files[i])
            child_process->open_files[i] = filedup(process->open_files[i]);
    child_process->current_directory = idup(process->current_directory);

    safestrcpy(child_process->name, process->name, sizeof(process->name));

    child_pid = child_process->pid;

    release(&child_process->lock);

    acquire(&wait_lock);
    child_process->parent = process;
    release(&wait_lock);

    acquire(&child_process->lock);
    child_process->state = RUNNABLE;
    release(&child_process->lock);

    return child_pid;
}

// processの子をinitに引き取らせる。
// 呼び出し元はwait_lockを保持していなければならない。
void reparent(struct proc *process)
{
    struct proc *child_process;

    for (child_process = processes;
         child_process < &processes[NPROC]; child_process++) {
        if (child_process->parent == process) {
            child_process->parent = init_process;
            wakeup(init_process);
        }
    }
}

// 現在のプロセスを終了する。戻らない。
// 終了したプロセスは親がwait()を呼ぶまでゾンビ状態に留まる。
void kexit(int status)
{
    struct proc *process = myproc();

    if (process == init_process)
        panic("init exiting");

    // オープン中の全ファイルを閉じる。
    for (int fd = 0; fd < NOFILE; fd++) {
        if (process->open_files[fd]) {
            struct file *file = process->open_files[fd];
            fileclose(file);
            process->open_files[fd] = 0;
        }
    }

    begin_op();
    iput(process->current_directory);
    end_op();
    process->current_directory = 0;

    acquire(&wait_lock);

    // 子をinitへ引き渡す。
    reparent(process);

    // 親はwait()でスリープしているかもしれない。
    wakeup(process->parent);

    acquire(&process->lock);

    process->exit_status = status;
    process->state = ZOMBIE;

    release(&wait_lock);

    // スケジューラへ移り、二度と戻らない。
    sched();
    panic("zombie exit");
}

// 子プロセスの終了を待ち、そのpidを返す。
// 子がいなければ-1を返す。
int kwait(uint64 status_address)
{
    struct proc *child_process;
    int has_children, child_pid;
    struct proc *process = myproc();

    acquire(&wait_lock);

    for (;;) {
        // 表を走査して終了した子を探す。
        has_children = 0;
        for (child_process = processes;
             child_process < &processes[NPROC]; child_process++) {
            if (child_process->parent == process) {
                // 子がまだexit()やswtch()の途中でないことを確認する。
                acquire(&child_process->lock);

                has_children = 1;
                if (child_process->state == ZOMBIE) {
                    // 見つかった。
                    child_pid = child_process->pid;
                    if (status_address != 0 &&
                        copyout(process->pagetable, process->memory_size, status_address,
                                (char *)&child_process->exit_status,
                                sizeof(child_process->exit_status)) < 0) {
                        release(&child_process->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    child_process->parent = 0;
                    freeproc(child_process);
                    release(&child_process->lock);
                    release(&wait_lock);
                    return child_pid;
                }
                release(&child_process->lock);
            }
        }

        // 子がいなければ待つ意味がない。
        if (!has_children || is_killed(process)) {
            release(&wait_lock);
            return -1;
        }

        // 子の終了を待つ。
        sleep_prepare(process); //DOC: wait-sleep
        release(&wait_lock);
        sleep();
        acquire(&wait_lock);
    }
}

// CPUごとのプロセススケジューラ。
// 各CPUは自身の設定後にscheduler()を呼ぶ。
// スケジューラは戻らず、次を繰り返す:
//  - 実行するプロセスを選ぶ。
//  - swtchでそのプロセスの実行を開始する。
//  - 最終的にそのプロセスがswtchでスケジューラへ制御を戻す。
void scheduler(void)
{
    struct proc *process;
    struct cpu *cpu = mycpu();

    cpu->process = 0;
    for (;;) {
        // 直前に実行したプロセスが割り込みを無効にしているかもしれない。
        // 全プロセスが待機している場合のデッドロックを避けるため有効にし、
        // 割り込みとwfiの競合を避けるため再び無効にする。
        intr_on();
        intr_off();

        int found_runnable_process = 0;
        for (process = processes; process < &processes[NPROC]; process++) {
            acquire(&process->lock);
            if (process->state == RUNNABLE) {
                // 選択したプロセスへ切り替える。
                // スケジューラへ戻る前にロックを解放し、再取得するのは
                // プロセスの役割である。
                process->state = RUNNING;
                cpu->process = process;
                swtch(&cpu->context, &process->context);

                // 解放時に割り込みを再有効化しない。
                mycpu()->interrupts_enabled_before_push = 0;

                // プロセスはひとまず実行を終えた。
                // 戻る前にprocess->stateを変更しているはずである。
                cpu->process = 0;
                found_runnable_process = 1;
            }
            release(&process->lock);
        }
        if (found_runnable_process == 0) {
            // 実行するものがないため、割り込みまでこのコアを停止する。
            asm volatile("wfi");
        }
    }
}

// スケジューラへ切り替える。process->lockだけを保持し、process->stateを
// 変更済みでなければならない。intenaはCPUではなくカーネルスレッドの
// 属性なので保存・復元する。本来はproc->interrupts_enabled_before_pushとproc->interrupt_disable_depthにすべきだが、
// プロセスがいないままロックを保持する少数の箇所で動かなくなる。
void sched(void)
{
    int interrupts_were_enabled;
    struct proc *process = myproc();

    if (!holding(&process->lock))
        panic("sched process->lock");
    if (mycpu()->interrupt_disable_depth != 1)
        panic("sched locks");
    if (process->state == RUNNING)
        panic("sched RUNNING");
    if (intr_get())
        panic("sched interruptible");

    interrupts_were_enabled = mycpu()->interrupts_enabled_before_push;
    swtch(&process->context, &mycpu()->context);
    mycpu()->interrupts_enabled_before_push = interrupts_were_enabled;
}

// 次のスケジューリングまでCPUを譲る。
void yield(void)
{
    struct proc *process = myproc();
    acquire(&process->lock);
    process->state = RUNNABLE;
    sched();
    release(&process->lock);
}

// forkされた子がscheduler()で初めてスケジュールされると、
// forkretへswtchする。
void forkret(void)
{
    extern char userret[];
    static int is_first_process = 1;
    struct proc *process = myproc();

    // schedulerから引き継いだprocess->lockを保持している。
    release(&process->lock);

    if (__atomic_load_n(&is_first_process, __ATOMIC_ACQUIRE)) {
        // ファイルシステムの初期化は通常のプロセスのコンテキストで
        // （たとえばsleepを呼ぶため）実行する必要があり、main()からは実行できない。
        fsinit(ROOTDEV);

        // 他のコアからis_first_process=0が見えるようにする。
        __atomic_store_n(&is_first_process, 0, __ATOMIC_RELEASE);

        // ファイルシステムを初期化したのでkexec()を呼び出せる。
        // kexecの戻り値(argc)をa0に入れる。
        process->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
        if (process->trapframe->a0 == -1) {
            panic("exec");
        }
    }

    // usertrap()から戻るときと同じようにユーザ空間へ戻る。
    prepare_return();
    uint64 satp = MAKE_SATP(process->pagetable);
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64))trampoline_userret)(satp);
}

// 現在のプロセスをchanでの起床待ちとして登録する。
void sleep_prepare(void *channel)
{
    struct proc *process = myproc();

    acquire(&process->lock);
    if (channel == 0)
        panic("sleep_prepare: zero chan");
    process->sleep_channel = channel;
    release(&process->lock);
}

// スレッドをスリープさせる。事前にsleep_prepare()が呼ばれたことを前提とする。
// sleep_prepare()で登録したチャネルがその間に起こされていれば、
// スリープせず直ちに戻る。
void sleep(void)
{
    struct proc *process = myproc();

    acquire(&process->lock);
    if (process->sleep_channel != 0) {
        process->state = SLEEPING;
        sched();
    }
    release(&process->lock);
}

// 指定チャネルでスリープしている全プロセスを起こす。
void wakeup(void *channel)
{
    struct proc *process;

    for (process = processes; process < &processes[NPROC]; process++) {
        acquire(&process->lock);
        if (process->sleep_channel == channel) {
            // プロセスがこのチャネルで起床待ちなら、process->sleep_channelを消して
            // 起床が発生したことを知らせる。
            process->sleep_channel = 0;

            // この待機プロセスが実際にスリープまで進んでいたら、
            // RUNNABLEへ戻す。
            if (process->state == SLEEPING) {
                process->state = RUNNABLE;
            }
        }
        release(&process->lock);
    }
}

// 指定されたpidのプロセスを終了させる。
// 対象プロセスはユーザ空間へ戻ろうとするまで終了しない
// （trap.cのusertrap()を参照）。
int kkill(int pid)
{
    struct proc *process;

    for (process = processes; process < &processes[NPROC]; process++) {
        acquire(&process->lock);
        if (process->pid == pid) {
            process->is_killed = 1;
            if (process->state == SLEEPING) {
                // sleep()中のプロセスを起こす。
                process->state = RUNNABLE;
            }
            release(&process->lock);
            return 0;
        }
        release(&process->lock);
    }
    return -1;
}

void mark_killed(struct proc *process)
{
    acquire(&process->lock);
    process->is_killed = 1;
    release(&process->lock);
}

int is_killed(struct proc *process)
{
    int process_is_killed;

    acquire(&process->lock);
    process_is_killed = process->is_killed;
    release(&process->lock);
    return process_is_killed;
}

// destination_is_userに応じてユーザまたはカーネルのアドレスへ複写する。
// 成功時は0、エラー時は-1を返す。
int either_copyout(int destination_is_user, uint64 destination_address,
                   void *source, uint64 byte_count)
{
    struct proc *process = myproc();
    if (destination_is_user) {
        return copyout(process->pagetable, process->memory_size,
                       destination_address, source, byte_count);
    } else {
        memmove((char *)destination_address, source, byte_count);
        return 0;
    }
}

// source_is_userに応じてユーザまたはカーネルのアドレスから複写する。
// 成功時は0、エラー時は-1を返す。
int either_copyin(void *destination, int source_is_user,
                  uint64 source_address, uint64 byte_count)
{
    struct proc *process = myproc();
    if (source_is_user) {
        return copyin(process->pagetable, process->memory_size, destination,
                      source_address, byte_count);
    } else {
        memmove(destination, (char *)source_address, byte_count);
        return 0;
    }
}

// デバッグ用にプロセス一覧をコンソールへ表示する。
// ユーザがコンソールで^Pを入力すると実行される。
// 停止したマシンをさらに固まらせないようロックは使わない。
void procdump(void)
{
    static char *states[] = {
        // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie"
        // clang-format on
    };
    struct proc *process;
    char *state;

    printk("\n");
    for (process = processes; process < &processes[NPROC]; process++) {
        if (process->state == UNUSED)
            continue;
        if (process->state >= 0 && process->state < NELEM(states) && states[process->state])
            state = states[process->state];
        else
            state = "???";
        printk("%d %s %s", process->pid, state, process->name);
        printk("\n");
    }
}
