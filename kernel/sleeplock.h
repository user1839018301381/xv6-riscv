// プロセス用の長期ロック
struct sleeplock {
    uint is_locked;
    struct spinlock spinlock;

    // デバッグ用:
    char *name; // ロック名。
    int owner_pid;
};
