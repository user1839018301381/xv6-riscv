// 相互排除スピンロック。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void initlock(struct spinlock *lock, char *name)
{
    lock->name = name;
    lock->is_locked = 0;
    lock->owning_cpu = 0;
}

// ロックを取得する。
// ロックを取得するまでループ（スピン）する。
void acquire(struct spinlock *lock)
{
    push_off(); // デッドロックを避けるため割り込みを無効にする。
    if (holding(lock))
        panic("acquire");

    // RISC-Vでは__atomic_exchange_nはアトミックな交換になる:
    //   a5 = 1
    //   s1 = &lock->is_locked
    //   amoswap.w.aq a5, a5, (s1)
    //
    // __atomic_exchange_nに__ATOMIC_ACQUIREを渡すと、
    // Cコンパイラとプロセッサはこの点を越えてロードやストアを移動しない。
    // これによりクリティカルセクションのメモリ参照が、
    // ロック取得後に厳密に行われる。
    while (__atomic_exchange_n(&lock->is_locked, 1, __ATOMIC_ACQUIRE) != 0)
        ;

    // holding()とデバッグ用にロック取得情報を記録する。
    lock->owning_cpu = mycpu();
}

// ロックを解放する。
void release(struct spinlock *lock)
{
    if (!holding(lock))
        panic("release");

    lock->owning_cpu = 0;

    // lock->is_locked = 0と同等の処理でロックを解放する。
    //
    // Cの代入は使わない。C標準では代入が複数のストア命令で
    // 実装される可能性があるためである。
    //
    // RISC-Vでは__atomic_store_nは単一のアトミックストアになる:
    //   s1 = &lock->is_locked
    //   fence rw,w
    //   sw zero,0(s1)
    //
    // __atomic_store_nに__ATOMIC_RELEASEを渡すと、
    // CコンパイラとCPUはこの点を越えてロードやストアを移動しない。
    // これによりクリティカルセクション内の全ストアがロック解放前に
    // 他のCPUから見えるようになり、クリティカルセクション内のロードが
    // ロック解放より前に厳密に行われる。
    //
    // RISC-Vではストアの前にフェンス命令が生成される:
    //   fence rw,w
    __atomic_store_n(&lock->is_locked, 0, __ATOMIC_RELEASE);

    pop_off();
}

// このCPUがロックを保持しているか確認する。
// 割り込みは無効でなければならない。
int holding(struct spinlock *lock)
{
    int is_held;
    is_held = (lock->is_locked && lock->owning_cpu == mycpu());
    return is_held;
}

// push_off/pop_offはintr_off()/intr_on()に似ているが、対になっている。
// 2回のpush_off()を元に戻すには2回のpop_off()が必要である。
// また、最初から割り込みが無効ならpush_off、pop_off後も無効のままである。

void push_off(void)
{
    // mycpu()使用中の意図しないコンテキスト切り替えを防ぐため、
    // 割り込みを無効にする。
    uint64 status_flags = rc_sstatus(SSTATUS_SIE);
    int interrupts_were_enabled = !!(status_flags & SSTATUS_SIE);

    if (mycpu()->interrupt_disable_depth == 0)
        mycpu()->interrupts_enabled_before_push = interrupts_were_enabled;
    mycpu()->interrupt_disable_depth += 1;
}

void pop_off(void)
{
    struct cpu *cpu = mycpu();
    if (intr_get())
        panic("pop_off - interruptible");
    if (cpu->interrupt_disable_depth < 1)
        panic("pop_off");
    cpu->interrupt_disable_depth -= 1;
    if (cpu->interrupt_disable_depth == 0 && cpu->interrupts_enabled_before_push)
        intr_on();
}
