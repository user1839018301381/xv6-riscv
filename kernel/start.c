#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S はCPUごとに1つのスタックを必要とする。
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// entry.Sはstack0を使い、マシンモードでここへジャンプする。
void start()
{
    // mret用にM Previous PrivilegeモードをSupervisorに設定する。
    unsigned long x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;
    w_mstatus(x);

    // mret用にM Exception Program Counterをmainに設定する。
    // gcc -mcmodel=medany が必要
    w_mepc((uint64)main);

    // 当面はページングを無効にする。
    w_satp(0);

    // すべての割り込みと例外をスーパーバイザモードに委譲する。
    w_medeleg(0xffff);
    w_mideleg(0xffff);
    w_sie(r_sie() | SIE_SEIE | SIE_STIE);

    // スーパーバイザモードに物理メモリ全体へのアクセスを与えるよう
    // Physical Memory Protectionを設定する。
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);

    // ページテーブルのAビットとDビットのハードウェア更新を有効にする。
    w_menvcfg(r_menvcfg() | MENVCFG_ADUE);

    // クロック割り込みを要求する。
    timerinit();

    // cpuid()用に各CPUのhartidをtpレジスタに保持する。
    int id = r_mhartid();
    w_tp(id);

    // スーパーバイザモードに切り替えてmain()へジャンプする。
    asm volatile("mret");
}

// 各hartにタイマ割り込みを生成させる。
void timerinit()
{
    // sstc拡張（すなわちstimecmp）を有効にする。
    w_menvcfg(r_menvcfg() | MENVCFG_STCE);

    // スーパーバイザがstimecmpとtimeを使えるようにする。
    w_mcounteren(r_mcounteren() | 2);

    // 最初のタイマ割り込みを要求する。
    w_stimecmp(r_time() + 1000000);
}
