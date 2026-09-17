#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.Sのコード

// wait()中の親プロセスへの起床通知が失われないようにする。
// p->parentを使う際のメモリモデルにも従えるようにする。
// どのp->lockよりも先に取得しなければならない。
struct spinlock wait_lock;

// 各プロセスのカーネルスタック用にページを割り当てる。
// メモリの高位にマップし、その後ろにアクセス不能なガードページを置く。
void proc_mapstacks(pagetable_t kpgtbl)
{
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++) {
        char *pa = kalloc();
        if (pa == 0)
            panic("kalloc");
        uint64 va = KSTACK((int)(p - proc));
        kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
}

// プロセス表を初期化する。
void procinit(void)
{
    struct proc *p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for (p = proc; p < &proc[NPROC]; p++) {
        initlock(&p->lock, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK((int)(p - proc));
    }
}

// プロセスが別のCPUへ移される競合を防ぐため、
// 割り込みを無効にして呼び出さなければならない。
int cpuid()
{
    int id = r_tp();
    return id;
}

// このCPUのcpu構造体を返す。
// 割り込みは無効でなければならない。
struct cpu *mycpu(void)
{
    int id = cpuid();
    struct cpu *c = &cpus[id];
    return c;
}

// 現在のstruct proc *を返す。なければ0を返す。
struct proc *myproc(void)
{
    push_off();
    struct cpu *c = mycpu();
    struct proc *p = c->proc;
    pop_off();
    return p;
}

int allocpid()
{
    int pid;

    acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);

    return pid;
}

// プロセス表からUNUSEDのプロセスを探す。
// 見つかったらカーネルで実行するための状態を初期化し、
// p->lockを保持した状態で返す。
// 空きプロセスがないかメモリ割り当てに失敗したら0を返す。
static struct proc *allocproc(void)
{
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->state == UNUSED) {
            goto found;
        } else {
            release(&p->lock);
        }
    }
    return 0;

found:
    p->pid = allocpid();
    p->state = USED;

    // トラップフレーム用ページを割り当てる。
    if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // 空のユーザページテーブル。
    p->pagetable = proc_pagetable(p);
    if (p->pagetable == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // forkretから実行を開始する新しいコンテキストを設定する。
    // forkretはユーザ空間へ戻る。
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;

    return p;
}

// proc構造体と、それに属するユーザページを含むデータを解放する。
// p->lockを保持していなければならない。
static void freeproc(struct proc *p)
{
    if (p->trapframe)
        kfree((void *)p->trapframe);
    p->trapframe = 0;
    if (p->pagetable)
        proc_freepagetable(p->pagetable, p->sz);
    p->pagetable = 0;
    p->sz = 0;
    p->pid = 0;
    p->name[0] = 0;
    p->chan = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = UNUSED;
}

// 指定されたプロセスのユーザページテーブルを作成する。
// ユーザメモリは持たないが、トランポリンとトラップフレームのページを持つ。
pagetable_t proc_pagetable(struct proc *p)
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
    if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
                 PTE_R | PTE_W) < 0) {
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return 0;
    }

    return pagetable;
}

// プロセスのページテーブルと、それが参照する物理メモリを解放する。
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, sz);
}

// 最初のユーザプロセスを設定する。
void userinit(void)
{
    struct proc *p;

    p = allocproc();
    initproc = p;

    p->cwd = namei("/");

    p->state = RUNNABLE;

    release(&p->lock);
}

// ユーザメモリをnバイト増減する。
// 成功時は0、失敗時は-1を返す。
int growproc(int n)
{
    uint64 sz;
    struct proc *p = myproc();

    sz = p->sz;
    if (n > 0) {
        if (sz + n > TRAPFRAME) {
            return -1;
        }
        if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
            return -1;
        }
    } else if (n < 0) {
        sz = uvmdealloc(p->pagetable, sz, sz + n);
    }
    p->sz = sz;
    return 0;
}

// 親を複製して新しいプロセスを作成する。
// 子のカーネルスタックを、fork()システムコールから戻った状態に設定する。
int kfork(void)
{
    int i, pid;
    struct proc *np;
    struct proc *p = myproc();

    // プロセスを割り当てる。
    if ((np = allocproc()) == 0) {
        return -1;
    }

    // 親のユーザメモリを子へ複写する。
    if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
        freeproc(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

    // 保存されたユーザレジスタを複写する。
    *(np->trapframe) = *(p->trapframe);

    // 子ではforkの戻り値が0になるようにする。
    np->trapframe->a0 = 0;

    // オープン中のファイルディスクリプタの参照カウントを増やす。
    for (i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = idup(p->cwd);

    safestrcpy(np->name, p->name, sizeof(p->name));

    pid = np->pid;

    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
    release(&np->lock);

    return pid;
}

// pの子をinitに引き取らせる。
// 呼び出し元はwait_lockを保持していなければならない。
void reparent(struct proc *p)
{
    struct proc *pp;

    for (pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->parent == p) {
            pp->parent = initproc;
            wakeup(initproc);
        }
    }
}

// 現在のプロセスを終了する。戻らない。
// 終了したプロセスは親がwait()を呼ぶまでゾンビ状態に留まる。
void kexit(int status)
{
    struct proc *p = myproc();

    if (p == initproc)
        panic("init exiting");

    // オープン中の全ファイルを閉じる。
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd]) {
            struct file *f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;

    acquire(&wait_lock);

    // 子をinitへ引き渡す。
    reparent(p);

    // 親はwait()でスリープしているかもしれない。
    wakeup(p->parent);

    acquire(&p->lock);

    p->xstate = status;
    p->state = ZOMBIE;

    release(&wait_lock);

    // スケジューラへ移り、二度と戻らない。
    sched();
    panic("zombie exit");
}

// 子プロセスの終了を待ち、そのpidを返す。
// 子がいなければ-1を返す。
int kwait(uint64 addr)
{
    struct proc *pp;
    int havekids, pid;
    struct proc *p = myproc();

    acquire(&wait_lock);

    for (;;) {
        // 表を走査して終了した子を探す。
        havekids = 0;
        for (pp = proc; pp < &proc[NPROC]; pp++) {
            if (pp->parent == p) {
                // 子がまだexit()やswtch()の途中でないことを確認する。
                acquire(&pp->lock);

                havekids = 1;
                if (pp->state == ZOMBIE) {
                    // 見つかった。
                    pid = pp->pid;
                    if (addr != 0 &&
                        copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                                sizeof(pp->xstate)) < 0) {
                        release(&pp->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    pp->parent = 0;
                    freeproc(pp);
                    release(&pp->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&pp->lock);
            }
        }

        // 子がいなければ待つ意味がない。
        if (!havekids || killed(p)) {
            release(&wait_lock);
            return -1;
        }

        // 子の終了を待つ。
        sleep_prepare(p); //DOC: wait-sleep
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
    struct proc *p;
    struct cpu *c = mycpu();

    c->proc = 0;
    for (;;) {
        // 直前に実行したプロセスが割り込みを無効にしているかもしれない。
        // 全プロセスが待機している場合のデッドロックを避けるため有効にし、
        // 割り込みとwfiの競合を避けるため再び無効にする。
        intr_on();
        intr_off();

        int found = 0;
        for (p = proc; p < &proc[NPROC]; p++) {
            acquire(&p->lock);
            if (p->state == RUNNABLE) {
                // 選択したプロセスへ切り替える。
                // スケジューラへ戻る前にロックを解放し、再取得するのは
                // プロセスの役割である。
                p->state = RUNNING;
                c->proc = p;
                swtch(&c->context, &p->context);

                // 解放時に割り込みを再有効化しない。
                mycpu()->intena = 0;

                // プロセスはひとまず実行を終えた。
                // 戻る前にp->stateを変更しているはずである。
                c->proc = 0;
                found = 1;
            }
            release(&p->lock);
        }
        if (found == 0) {
            // 実行するものがないため、割り込みまでこのコアを停止する。
            asm volatile("wfi");
        }
    }
}

// スケジューラへ切り替える。p->lockだけを保持し、proc->stateを
// 変更済みでなければならない。intenaはCPUではなくカーネルスレッドの
// 属性なので保存・復元する。本来はproc->intenaとproc->noffにすべきだが、
// プロセスがいないままロックを保持する少数の箇所で動かなくなる。
void sched(void)
{
    int intena;
    struct proc *p = myproc();

    if (!holding(&p->lock))
        panic("sched p->lock");
    if (mycpu()->noff != 1)
        panic("sched locks");
    if (p->state == RUNNING)
        panic("sched RUNNING");
    if (intr_get())
        panic("sched interruptible");

    intena = mycpu()->intena;
    swtch(&p->context, &mycpu()->context);
    mycpu()->intena = intena;
}

// 次のスケジューリングまでCPUを譲る。
void yield(void)
{
    struct proc *p = myproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    sched();
    release(&p->lock);
}

// forkされた子がscheduler()で初めてスケジュールされると、
// forkretへswtchする。
void forkret(void)
{
    extern char userret[];
    static int first = 1;
    struct proc *p = myproc();

    // schedulerから引き継いだp->lockを保持している。
    release(&p->lock);

    if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
        // ファイルシステムの初期化は通常のプロセスのコンテキストで
        // （たとえばsleepを呼ぶため）実行する必要があり、main()からは実行できない。
        fsinit(ROOTDEV);

        // 他のコアからfirst=0が見えるようにする。
        __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

        // ファイルシステムを初期化したのでkexec()を呼び出せる。
        // kexecの戻り値(argc)をa0に入れる。
        p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
        if (p->trapframe->a0 == -1) {
            panic("exec");
        }
    }

    // usertrap()から戻るときと同じようにユーザ空間へ戻る。
    prepare_return();
    uint64 satp = MAKE_SATP(p->pagetable);
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64))trampoline_userret)(satp);
}

// 現在のプロセスをchanでの起床待ちとして登録する。
void sleep_prepare(void *chan)
{
    struct proc *p = myproc();

    acquire(&p->lock);
    if (chan == 0)
        panic("sleep_prepare: zero chan");
    p->chan = chan;
    release(&p->lock);
}

// スレッドをスリープさせる。事前にsleep_prepare()が呼ばれたことを前提とする。
// sleep_prepare()で登録したチャネルがその間に起こされていれば、
// スリープせず直ちに戻る。
void sleep(void)
{
    struct proc *p = myproc();

    acquire(&p->lock);
    if (p->chan != 0) {
        p->state = SLEEPING;
        sched();
    }
    release(&p->lock);
}

// チャネルchanでスリープしている全プロセスを起こす。
void wakeup(void *chan)
{
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->chan == chan) {
            // プロセスがこのチャネルで起床待ちなら、p->chanを消して
            // 起床が発生したことを知らせる。
            p->chan = 0;

            // この待機プロセスが実際にスリープまで進んでいたら、
            // RUNNABLEへ戻す。
            if (p->state == SLEEPING) {
                p->state = RUNNABLE;
            }
        }
        release(&p->lock);
    }
}

// 指定されたpidのプロセスを終了させる。
// 対象プロセスはユーザ空間へ戻ろうとするまで終了しない
// （trap.cのusertrap()を参照）。
int kkill(int pid)
{
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            p->killed = 1;
            if (p->state == SLEEPING) {
                // sleep()中のプロセスを起こす。
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

void setkilled(struct proc *p)
{
    acquire(&p->lock);
    p->killed = 1;
    release(&p->lock);
}

int killed(struct proc *p)
{
    int k;

    acquire(&p->lock);
    k = p->killed;
    release(&p->lock);
    return k;
}

// usr_dstに応じてユーザアドレスまたはカーネルアドレスへ複写する。
// 成功時は0、エラー時は-1を返す。
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
    struct proc *p = myproc();
    if (user_dst) {
        return copyout(p->pagetable, p->sz, dst, src, len);
    } else {
        memmove((char *)dst, src, len);
        return 0;
    }
}

// usr_srcに応じてユーザアドレスまたはカーネルアドレスから複写する。
// 成功時は0、エラー時は-1を返す。
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
    struct proc *p = myproc();
    if (user_src) {
        return copyin(p->pagetable, p->sz, dst, src, len);
    } else {
        memmove(dst, (char *)src, len);
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
    struct proc *p;
    char *state;

    printk("\n");
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        printk("%d %s %s", p->pid, state, p->name);
        printk("\n");
    }
}
