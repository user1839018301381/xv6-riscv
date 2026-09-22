//
// 16550a UART用の低レベルドライバ。
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"

// UART制御レジスタはアドレスUART0にメモリマップされる。
// このマクロはレジスタの1つのアドレスを返す。
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))

#define ReadReg(reg)     (*(Reg(reg)))
#define WriteReg(reg, value) (*(Reg(reg)) = (value))

// UART制御レジスタ。
// 読み込みと書き込みで意味が異なるものがある。
// http://byterunner.com/16550.html を参照。
#define RHR             0        // 受信保持レジスタ（入力バイト用）
#define THR             0        // 送信保持レジスタ（出力バイト用）
#define IER             1        // 割り込み有効レジスタ
#define IER_RX_ENABLE   (1 << 0) // 受信割り込み
#define IER_TX_ENABLE   (1 << 1) // 送信割り込み
#define FCR             2        // FIFO制御レジスタ
#define FCR_FIFO_ENABLE (1 << 0)
#define FCR_FIFO_CLEAR  (3 << 1) // 2つのFIFOの内容を消去する
#define ISR             2        // 割り込みステータスレジスタ
#define LCR             3        // ライン制御レジスタ
#define LCR_EIGHT_BITS  (3 << 0)
#define LCR_BAUD_LATCH  (1 << 7) // ボーレート設定用の特殊モード
#define LSR             5        // ラインステータスレジスタ
#define LSR_RX_READY    (1 << 0) // RHRから読む入力が待機中
#define LSR_TX_IDLE     (1 << 5) // THRが次の送信文字を受け付け可能

// 送信スレッドの書き込みを直列化する
static struct sleeplock tx_lock;
static int tx_channel; // &tx_channelは「待機チャネル」

extern volatile int panicking; // printk.cから
extern volatile int panicked;  // printk.cから

void uartinit(void)
{
    // 割り込みを無効にする。
    WriteReg(IER, 0x00);

    // ボーレート設定用の特殊モード。
    WriteReg(LCR, LCR_BAUD_LATCH);

    // 38.4Kボーレートの下位バイト。
    WriteReg(0, 0x03);

    // 38.4Kボーレートの上位バイト。
    WriteReg(1, 0x00);

    // ボーレート設定モードを終了し、
    // ワード長を8ビット、パリティなしに設定する。
    WriteReg(LCR, LCR_EIGHT_BITS);

    // FIFOをリセットして有効にする。
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

    // 送信・受信割り込みを有効にする。
    WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

    initsleeplock(&tx_lock, "uart");
}

// buffer[]をUARTへ送信する。UARTがビジーならブロックするため、
// 割り込みからは呼べず、write()システムコールからのみ呼べる。
void uartwrite(char buffer[], int byte_count)
{
    acquiresleep(&tx_lock);

    int bytes_written = 0;
    while (bytes_written < byte_count) {
        sleep_prepare(&tx_channel);
        if (ReadReg(LSR) & LSR_TX_IDLE) {
            WriteReg(THR, buffer[bytes_written]);
            bytes_written++;
        } else {
            sleep();
        }
    }

    releasesleep(&tx_lock);
}

// 割り込みを使わずにUARTへ1バイトを書き込む。
// カーネルのprintk()と文字のエコーに使う。
// UARTの出力レジスタが空くまでスピンする。
void uartputc_sync(int character)
{
    if (panicking == 0)
        push_off();

    if (panicked) {
        for (;;)
            ;
    }

    // UARTがLSRのTransmit Holding Emptyを設定するまで待つ。
    while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
        ;
    WriteReg(THR, character);

    if (panicking == 0)
        pop_off();
}

// UARTから入力文字を1つ読み取ろうとする。
// 待機中の文字がなければ-1を返す。
static int uart_try_read_character(void)
{
    // 入力は準備できているか?
    if (ReadReg(LSR) & LSR_RX_READY) {
        return ReadReg(RHR);
    } else {
        return -1;
    }
}

// 入力の到着、さらなる出力の準備完了、またはその両方で発生した
// UART割り込みを処理する。devintr()から呼ばれる。
void uartintr(void)
{
    ReadReg(ISR); // 割り込みを確認する

    if (ReadReg(LSR) & LSR_TX_IDLE) {
        // UARTの送信が完了したので送信スレッドを起こす。
        wakeup(&tx_channel);
    }

    // 到着した文字があれば読み取って処理する。
    while (1) {
        int input_character = uart_try_read_character();
        if (input_character == -1)
            break;
        consoleintr(input_character);
    }
}
