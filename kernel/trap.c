#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// kernelvec.Sからkerneltrap()を呼び出す。
void kernelvec();

extern int devintr();

void trapinit(void) { initlock(&tickslock, "time"); }

// カーネル内で例外とトラップを受け取れるよう設定する。
void trapinithart(void) { w_stvec((uint64)kernelvec); }

//
// ユーザ空間からの割り込み、例外、システムコールを処理する。
// trampoline.Sから呼ばれ、trampoline.Sへ戻る。
// 戻り値はtrampoline.Sが切り替えるユーザsatpである。
//
uint64 usertrap(void)
{
    int interrupt_type = 0;

    if ((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // 現在はカーネルにいるため、割り込みと例外をkerneltrap()へ送る。
    w_stvec((uint64)kernelvec); //DOC: kernelvec

    struct proc *process = myproc();

    // ユーザプログラムカウンタを保存する。
    process->trapframe->epc = r_sepc();

    if (r_scause() == 8) {
        // システムコール

        if (is_killed(process))
            kexit(-1);

        // sepcはecall命令を指しているが、次の命令へ戻したい。
        process->trapframe->epc += 4;

        // 割り込みでsepc、scause、sstatusが変わるため、
        // これらのレジスタを使い終わった今だけ有効にする。
        intr_on();

        syscall();
    } else if ((interrupt_type = devintr()) != 0) {
        // 正常
    } else if ((r_scause() == 15 || r_scause() == 13) &&
               vmfault(process->pagetable, process->memory_size, r_stval(),
                       (r_scause() == 13) ? 1 : 0) != 0) {
        // 遅延割り当てページでのページフォルト
    } else {
        printk("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(),
               process->pid);
        printk("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
        mark_killed(process);
    }

    if (is_killed(process))
        kexit(-1);

    // タイマ割り込みならCPUを譲る。
    if (interrupt_type == 2)
        yield();

    prepare_return();

    // trampoline.Sが切り替えるユーザページテーブル
    uint64 satp = MAKE_SATP(process->pagetable);

    // trampoline.Sへ戻る。satpの値はa0に入れる。
    return satp;
}

//
// ユーザ空間へ戻るためにトラップフレームと制御レジスタを設定する
//
void prepare_return(void)
{
    struct proc *process = myproc();

    // トラップの行き先をkerneltrap()からusertrap()へ切り替えようとしている。
    // カーネルコードからusertrap()へトラップすると危険なので、割り込みを無効にする。
    intr_off();

    // システムコール、割り込み、例外をtrampoline.Sのuservecへ送る。
    uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    w_stvec(trampoline_uservec);

    // プロセスが次にカーネルへトラップしたときuservecが必要とする
    // トラップフレームの値を設定する。
    process->trapframe->kernel_satp = r_satp();
    process->trapframe->kernel_sp = process->kernel_stack + PGSIZE;
    process->trapframe->kernel_trap = (uint64)usertrap;
    process->trapframe->kernel_hartid = r_tp();

    // trampoline.Sのsretがユーザ空間へ移るために使うレジスタを設定する。

    // S Previous PrivilegeモードをUserに設定する。
    unsigned long supervisor_status = r_sstatus();
    supervisor_status &= ~SSTATUS_SPP; // ユーザモード用にSPPを0へクリアする
    supervisor_status |= SSTATUS_SPIE; // ユーザモードで割り込みを有効にする
    w_sstatus(supervisor_status);

    // S Exception Program Counterを保存したユーザpcに設定する。
    w_sepc(process->trapframe->epc);
}

// カーネルコードからの割り込みと例外は、現在のカーネルスタック上で
// kernelvec経由でここへ来る。
void kerneltrap()
{
    int interrupt_type = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    if ((interrupt_type = devintr()) == 0) {
        // 不明な原因からの割り込みまたはトラップ
        printk("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(),
               r_stval());
        panic("kerneltrap");
    }

    // タイマ割り込みならCPUを譲る。
    if (interrupt_type == 2 && myproc() != 0)
        yield();

    // yield()でトラップが発生した可能性があるため、
    // kernelvec.Sのsepc命令が使うトラップレジスタを復元する。
    w_sepc(sepc);
    w_sstatus(sstatus);
}

void clockintr()
{
    if (cpuid() == 0) {
        acquire(&tickslock);
        ticks++;
        wakeup(&ticks);
        release(&tickslock);
    }

    // 次のタイマ割り込みを要求する。これにより割り込み要求も消える。
    // 1000000は約0.1秒である。
    w_stimecmp(r_time() + 1000000);
}

// 外部割り込みまたはソフトウェア割り込みか確認して処理する。
// タイマ割り込みなら2、他のデバイスなら1、認識できなければ0を返す。
int devintr()
{
    uint64 scause = r_scause();

    if (scause == 0x8000000000000009L) {
        // PLIC経由のスーパーバイザ外部割り込みである。

        // irqは割り込みを発生させたデバイスを示す。
        int irq = plic_claim();

        if (irq == UART0_IRQ) {
            uartintr();
        } else if (irq == VIRTIO0_IRQ) {
            virtio_disk_intr();
        } else if (irq) {
            printk("unexpected interrupt irq=%d\n", irq);
        }

        // PLICは各デバイスが一度に発生させられる割り込みを最大1つに制限する。
        // そのデバイスが再び割り込めるようになったことをPLICに伝える。
        if (irq)
            plic_complete(irq);

        return 1;
    } else if (scause == 0x8000000000000005L) {
        // タイマ割り込み。
        clockintr();
        return 2;
    } else {
        return 0;
    }
}
