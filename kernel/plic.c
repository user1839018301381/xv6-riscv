#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// RISC-Vプラットフォームレベル割り込みコントローラ(PLIC)。
//

void plicinit(void)
{
    // 必要なIRQの優先度を0以外に設定する（0なら無効）。
    *(uint32 *)(PLIC + UART0_IRQ * 4) = 1;
    *(uint32 *)(PLIC + VIRTIO0_IRQ * 4) = 1;
}

void plicinithart(void)
{
    int hart = cpuid();

    // このhartのSモードでUARTとvirtioディスクの
    // 有効ビットを設定する。
    *(uint32 *)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

    // このhartのSモードの優先度しきい値を0に設定する。
    *(uint32 *)PLIC_SPRIORITY(hart) = 0;
}

// 処理すべき割り込みをPLICに問い合わせる。
int plic_claim(void)
{
    int hart = cpuid();
    int irq = *(uint32 *)PLIC_SCLAIM(hart);
    return irq;
}

// このIRQを処理したことをPLICに伝える。
void plic_complete(int irq)
{
    int hart = cpuid();
    *(uint32 *)PLIC_SCLAIM(hart) = irq;
}
