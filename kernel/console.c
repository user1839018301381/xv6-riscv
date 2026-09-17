//
// uartへのコンソール入出力。
// 読み込みは1行単位。
// 特殊入力文字の実装:
//   改行 -- 行末
//   control-h -- バックスペース
//   control-u -- 行削除
//   control-d -- ファイル終端
//   control-p -- プロセス一覧表示
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100       // 最後に出力した1文字を消去する
#define C(x)      ((x) - '@') // Control-xを表す

//
// uartへ1文字送る。割り込みやsleep()を使わないため
// 割り込みから呼んでも安全。たとえばprintkや入力文字の
// エコー用に使われる。
//
void consputc(int c)
{
    if (c == BACKSPACE) {
        // バックスペースが入力されたら空白で上書きする。
        uartputc_sync('\b');
        uartputc_sync(' ');
        uartputc_sync('\b');
    } else {
        uartputc_sync(c);
    }
}

struct {
    struct spinlock lock;

    // 入力用循環バッファ
#define INPUT_BUF_SIZE 128
    char buf[INPUT_BUF_SIZE];
    uint r; // 読み出し位置
    uint w; // 書き込み位置
    uint e; // 編集位置
} cons;

//
// コンソールへのユーザのwrite()システムコールはここで処理する。
// sleep()とUART割り込みを使う。
//
int consolewrite(int user_src, uint64 src, int n)
{
    char buf[32]; // ユーザ空間からuartへまとめて運ぶ用。
    int i = 0;

    while (i < n) {
        int nn = sizeof(buf);
        if (nn > n - i)
            nn = n - i;
        if (either_copyin(buf, user_src, src + i, nn) == -1)
            break;
        uartwrite(buf, nn);
        i += nn;
    }

    return i;
}

//
// コンソールからのユーザのread()はここで処理する。
// 入力1行分(まで)をdstに複写する。
// user_dstはdstがユーザアドレスかカーネルアドレスかを示す。
//
int consoleread(int user_dst, uint64 dst, int n)
{
    uint target;
    int c;
    char cbuf;

    target = n;
    acquire(&cons.lock);
    while (n > 0) {
        // 割り込みハンドラがcons.bufferに入力を入れるまで待つ。
        while (cons.r == cons.w) {
            if (killed(myproc())) {
                release(&cons.lock);
                return -1;
            }
            sleep_prepare(&cons.r);
            release(&cons.lock);
            sleep();
            acquire(&cons.lock);
        }

        c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

        if (c == C('D')) { // ファイル終端
            if (n < target) {
                // 次回用に^Dを残し、呼び出し元が0バイト結果を得られるようにする。
                cons.r--;
            }
            break;
        }

        // 入力1バイトをユーザ空間バッファに複写する。
        cbuf = c;
        if (either_copyout(user_dst, dst, &cbuf, 1) == -1)
            break;

        dst++;
        --n;

        if (c == '\n') {
            // 1行分が揃ったのでユーザレベルのread()に戻る。
            break;
        }
    }
    release(&cons.lock);

    return target - n;
}

//
// コンソール入力の割り込みハンドラ。
// uartintr()が入力1文字ごとに呼ぶ。
// 消去や行削除を処理してcons.bufに追加し、
// 1行揃ったらconsoleread()を起こす。
//
void consoleintr(int c)
{
    acquire(&cons.lock);

    switch (c) {
    case C('P'): // プロセス一覧を表示する。
        procdump();
        break;
    case C('U'): // 行全体を削除する。
        while (cons.e != cons.w &&
               cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] != '\n') {
            cons.e--;
            consputc(BACKSPACE);
        }
        break;
    case C('H'): // バックスペース
    case '\x7f': // Deleteキー
        if (cons.e != cons.w) {
            cons.e--;
            consputc(BACKSPACE);
        }
        break;
    default:
        if (c != 0 && cons.e - cons.r < INPUT_BUF_SIZE) {
            c = (c == '\r') ? '\n' : c;

            // ユーザに入力文字をエコーバックする。
            consputc(c);

            // consoleread()が消費できるよう保存する。
            cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

            if (c == '\n' || c == C('D') || cons.e - cons.r == INPUT_BUF_SIZE) {
                // 1行分(またはファイル終端)が揃ったらconsoleread()を起こす。
                cons.w = cons.e;
                wakeup(&cons.r);
            }
        }
        break;
    }

    release(&cons.lock);
}

void consoleinit(void)
{
    initlock(&cons.lock, "cons");

    uartinit();

    // read/writeシステムコールをconsoleread/consolewriteにつなげる。
    devsw[CONSOLE].read = consoleread;
    devsw[CONSOLE].write = consolewrite;
}
