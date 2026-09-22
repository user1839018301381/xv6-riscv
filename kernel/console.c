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
#define C(character) ((character) - '@') // Control-xを表す

//
// uartへ1文字送る。割り込みやsleep()を使わないため
// 割り込みから呼んでも安全。たとえばprintkや入力文字の
// エコー用に使われる。
//
void consputc(int character)
{
    if (character == BACKSPACE) {
        // バックスペースが入力されたら空白で上書きする。
        uartputc_sync('\b');
        uartputc_sync(' ');
        uartputc_sync('\b');
    } else {
        uartputc_sync(character);
    }
}

struct {
    struct spinlock lock;

    // 入力用循環バッファ
#define INPUT_BUF_SIZE 128
    char buffer[INPUT_BUF_SIZE];
    uint read_index;
    uint write_index;
    uint edit_index;
} console_input;

//
// コンソールへのユーザのwrite()システムコールはここで処理する。
// sleep()とUART割り込みを使う。
//
int consolewrite(int source_is_user, uint64 source_address, int byte_count)
{
    char buffer[32]; // ユーザ空間からuartへまとめて運ぶ用。
    int bytes_written = 0;

    while (bytes_written < byte_count) {
        int chunk_byte_count = sizeof(buffer);
        if (chunk_byte_count > byte_count - bytes_written)
            chunk_byte_count = byte_count - bytes_written;
        if (either_copyin(buffer, source_is_user,
                          source_address + bytes_written,
                          chunk_byte_count) == -1)
            break;
        uartwrite(buffer, chunk_byte_count);
        bytes_written += chunk_byte_count;
    }

    return bytes_written;
}

//
// コンソールからのユーザのread()はここで処理する。
// 入力1行分(まで)をdestination_addressに複写する。
// destination_is_userは複写先がユーザアドレスかを示す。
//
int consoleread(int destination_is_user, uint64 destination_address,
                int byte_count)
{
    uint requested_byte_count;
    int character;
    char input_byte;

    requested_byte_count = byte_count;
    acquire(&console_input.lock);
    while (byte_count > 0) {
        // 割り込みハンドラがconsole_input.bufferに入力を入れるまで待つ。
        while (console_input.read_index == console_input.write_index) {
            if (is_killed(myproc())) {
                release(&console_input.lock);
                return -1;
            }
            sleep_prepare(&console_input.read_index);
            release(&console_input.lock);
            sleep();
            acquire(&console_input.lock);
        }

        character = console_input.buffer[
            console_input.read_index++ % INPUT_BUF_SIZE];

        if (character == C('D')) { // ファイル終端
            if (byte_count < requested_byte_count) {
                // 次回用に^Dを残し、呼び出し元が0バイト結果を得られるようにする。
                console_input.read_index--;
            }
            break;
        }

        // 入力1バイトをユーザ空間バッファに複写する。
        input_byte = character;
        if (either_copyout(destination_is_user, destination_address,
                           &input_byte, 1) == -1)
            break;

        destination_address++;
        --byte_count;

        if (character == '\n') {
            // 1行分が揃ったのでユーザレベルのread()に戻る。
            break;
        }
    }
    release(&console_input.lock);

    return requested_byte_count - byte_count;
}

//
// コンソール入力の割り込みハンドラ。
// uartintr()が入力1文字ごとに呼ぶ。
// 消去や行削除を処理してcons.bufに追加し、
// 1行揃ったらconsoleread()を起こす。
//
void consoleintr(int character)
{
    acquire(&console_input.lock);

    switch (character) {
    case C('P'): // プロセス一覧を表示する。
        procdump();
        break;
    case C('U'): // 行全体を削除する。
        while (console_input.edit_index != console_input.write_index &&
               console_input.buffer[(console_input.edit_index - 1) %
                                    INPUT_BUF_SIZE] != '\n') {
            console_input.edit_index--;
            consputc(BACKSPACE);
        }
        break;
    case C('H'): // バックスペース
    case '\x7f': // Deleteキー
        if (console_input.edit_index != console_input.write_index) {
            console_input.edit_index--;
            consputc(BACKSPACE);
        }
        break;
    default:
        if (character != 0 &&
            console_input.edit_index - console_input.read_index <
                INPUT_BUF_SIZE) {
            character = (character == '\r') ? '\n' : character;

            // ユーザに入力文字をエコーバックする。
            consputc(character);

            // consoleread()が消費できるよう保存する。
            console_input.buffer[
                console_input.edit_index++ % INPUT_BUF_SIZE] = character;

            if (character == '\n' || character == C('D') ||
                console_input.edit_index - console_input.read_index ==
                    INPUT_BUF_SIZE) {
                // 1行分(またはファイル終端)が揃ったらconsoleread()を起こす。
                console_input.write_index = console_input.edit_index;
                wakeup(&console_input.read_index);
            }
        }
        break;
    }

    release(&console_input.lock);
}

void consoleinit(void)
{
    initlock(&console_input.lock, "console_input");

    uartinit();

    // read/writeシステムコールをconsoleread/consolewriteにつなげる。
    devsw[CONSOLE].read = consoleread;
    devsw[CONSOLE].write = consolewrite;
}
