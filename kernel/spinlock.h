// 相互排除ロック。
struct spinlock {
    uint is_locked;

    // デバッグ用:
    char *name;      // ロック名。
    struct cpu *owning_cpu;
};
