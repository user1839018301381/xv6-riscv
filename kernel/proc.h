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
    struct proc *proc;      // このCPU上で実行中のプロセス、または空。
    struct context context; // スケジューラへ入るためにswtch()する先。
    int noff;               // push_off()の入れ子の深さ。
    int intena;             // push_off()前に割込みは有効だったか?
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

    // これらを使うときはp->lockを保持しなければならない:
    enum procstate state; // プロセスの状態
    void *chan;           // 0でなければchanでスリープ中
    int killed;           // 0でなければ終了対象
    int xstate;           // 親のwaitへ返す終了ステータス
    int pid;              // プロセスID

    // これを使うときはwait_lockを保持しなければならない:
    struct proc *parent; // 親プロセス

    // これらはプロセス固有なのでp->lockを保持する必要はない。
    uint64 kstack;               // カーネルスタックの仮想アドレス
    uint64 sz;                   // プロセスメモリのサイズ（バイト）
    pagetable_t pagetable;       // ユーザページテーブル
    struct trapframe *trapframe; // trampoline.S用のデータページ
    struct context context;      // プロセス実行のためにここへswtchする
    struct file *ofile[NOFILE];  // オープン中のファイル
    struct inode *cwd;           // 現在のディレクトリ
    char name[16];               // プロセス名（デバッグ用）
};
