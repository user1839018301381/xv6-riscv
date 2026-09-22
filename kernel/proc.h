// カーネルコンテキスト切替え用の保存レジスタ。
struct context {
    uint64 ra;
    uint64 sp;

    // 被呼び出し側保存レジスタ
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

// CPUごとの状態。
struct cpu {
    struct proc *process;    // このCPU上で実行中のプロセス、または空。
    struct context context;  // スケジューラへ入るためにswtch()する先。
    int interrupt_disable_depth;
    int interrupts_enabled_before_push;
};

extern struct cpu cpus[NCPU];

// trampoline.S内のトラップ処理コード用プロセスごとのデータ。
// ユーザページテーブル内でトランポリンページ直下の1ページを単独で使う。
// カーネルページテーブルには特別に割当てない。
// trampoline.Sのuservecはユーザレジスタをtrapframeに保存し、
// trapframeのkernel_sp・kernel_hartid・kernel_satpからレジスタを初期化してkernel_trapへ跳ぶ。
// prepare_return()とtrampoline.Sのuserretはtrapframeのkernel_*を設定し、
// trapframeからユーザレジスタを復元し、ユーザページテーブルに切替えてユーザ空間に入る。
struct trapframe {
    /*   0 */ uint64 kernel_satp;   // カーネルページテーブル
    /*   8 */ uint64 kernel_sp;     // プロセスのカーネルスタックの先頭
    /*  16 */ uint64 kernel_trap;   // usertrap()
    /*  24 */ uint64 epc;           // 保存されたユーザプログラムカウンタ
    /*  32 */ uint64 kernel_hartid; // 保存されたカーネルのtp
    /*  40 */ uint64 ra;
    /*  48 */ uint64 sp;
    /*  56 */ uint64 gp;
    /*  64 */ uint64 tp;
    /*  72 */ uint64 t0;
    /*  80 */ uint64 t1;
    /*  88 */ uint64 t2;
    /*  96 */ uint64 s0;
    /* 104 */ uint64 s1;
    /* 112 */ uint64 a0;
    /* 120 */ uint64 a1;
    /* 128 */ uint64 a2;
    /* 136 */ uint64 a3;
    /* 144 */ uint64 a4;
    /* 152 */ uint64 a5;
    /* 160 */ uint64 a6;
    /* 168 */ uint64 a7;
    /* 176 */ uint64 s2;
    /* 184 */ uint64 s3;
    /* 192 */ uint64 s4;
    /* 200 */ uint64 s5;
    /* 208 */ uint64 s6;
    /* 216 */ uint64 s7;
    /* 224 */ uint64 s8;
    /* 232 */ uint64 s9;
    /* 240 */ uint64 s10;
    /* 248 */ uint64 s11;
    /* 256 */ uint64 t3;
    /* 264 */ uint64 t4;
    /* 272 */ uint64 t5;
    /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// プロセスごとの状態
struct proc {
    struct spinlock lock;

    // これらを使うときはprocess->lockを保持しなければならない:
    enum procstate state; // プロセスの状態
    void *sleep_channel;
    int is_killed;
    int exit_status;
    int pid;              // プロセスID

    // これを使うときはwait_lockを保持しなければならない:
    struct proc *parent; // 親プロセス

    // これらはプロセス固有なのでprocess->lockを保持する必要はない。
    uint64 kernel_stack;
    uint64 memory_size;
    pagetable_t pagetable;        // ユーザページテーブル
    struct trapframe *trapframe;  // trampoline.S用のデータページ
    struct context context;       // プロセス実行のためにここへswtchする
    struct file *open_files[NOFILE];
    struct inode *current_directory;
    char name[16];                // プロセス名（デバッグ用）
};
