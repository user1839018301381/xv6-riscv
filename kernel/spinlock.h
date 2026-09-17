// 相互排除ロック。
struct spinlock {
    uint locked; // ロックは保持されているか?

    // デバッグ用:
    char *name;      // ロック名。
    struct cpu *cpu; // ロックを保持しているCPU。
};
