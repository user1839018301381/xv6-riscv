// プロセス用の長期ロック
struct sleeplock {
    uint locked;        // ロックは保持されているか?
    struct spinlock lk; // このスリープロックを保護するスピンロック

    // デバッグ用:
    char *name; // ロック名。
    int pid;    // ロックを保持しているプロセス
};
