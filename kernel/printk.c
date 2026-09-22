//
// 書式付きコンソール出力 -- printk、panic。
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

volatile int panicking = 0; // panicメッセージを出力中
volatile int panicked = 0;  // panicの最後で永遠にループ中

// 複数のprintk出力が混ざらないようにするロック。
static struct {
    struct spinlock lock;
} print_state;

static char digits[] = "0123456789abcdef";

static void printint(long long value, int base, int is_signed)
{
    char digit_buffer[20];
    int digit_index;
    int is_negative = is_signed && value < 0;
    unsigned long long magnitude;

    if (is_negative)
        magnitude = -value;
    else
        magnitude = value;

    digit_index = 0;
    do {
        digit_buffer[digit_index++] = digits[magnitude % base];
    } while ((magnitude /= base) != 0);

    if (is_negative)
        digit_buffer[digit_index++] = '-';

    while (--digit_index >= 0)
        consputc(digit_buffer[digit_index]);
}

static void printptr(uint64 pointer_value)
{
    int i;
    consputc('0');
    consputc('x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, pointer_value <<= 4)
        consputc(digits[pointer_value >> (sizeof(uint64) * 8 - 4)]);
}

// コンソールへ出力する。
int printk(char *format, ...)
{
    va_list arguments;
    int i, current_character;
    int specifier_first, specifier_second, specifier_third;
    char *string;

    if (panicking == 0)
        acquire(&print_state.lock);

    va_start(arguments, format);
    for (i = 0; (current_character = format[i] & 0xff) != 0; i++) {
        if (current_character != '%') {
            consputc(current_character);
            continue;
        }
        i++;
        specifier_first = format[i] & 0xff;
        specifier_second = specifier_third = 0;
        if (specifier_first)
            specifier_second = format[i + 1] & 0xff;
        if (specifier_second)
            specifier_third = format[i + 2] & 0xff;
        if (specifier_first == 'd') {
            printint(va_arg(arguments, int), 10, 1);
        } else if (specifier_first == 'l' && specifier_second == 'd') {
            printint(va_arg(arguments, uint64), 10, 1);
            i += 1;
        } else if (specifier_first == 'l' && specifier_second == 'l' &&
                   specifier_third == 'd') {
            printint(va_arg(arguments, uint64), 10, 1);
            i += 2;
        } else if (specifier_first == 'u') {
            printint(va_arg(arguments, uint32), 10, 0);
        } else if (specifier_first == 'l' && specifier_second == 'u') {
            printint(va_arg(arguments, uint64), 10, 0);
            i += 1;
        } else if (specifier_first == 'l' && specifier_second == 'l' &&
                   specifier_third == 'u') {
            printint(va_arg(arguments, uint64), 10, 0);
            i += 2;
        } else if (specifier_first == 'x') {
            printint(va_arg(arguments, uint32), 16, 0);
        } else if (specifier_first == 'l' && specifier_second == 'x') {
            printint(va_arg(arguments, uint64), 16, 0);
            i += 1;
        } else if (specifier_first == 'l' && specifier_second == 'l' &&
                   specifier_third == 'x') {
            printint(va_arg(arguments, uint64), 16, 0);
            i += 2;
        } else if (specifier_first == 'p') {
            printptr(va_arg(arguments, uint64));
        } else if (specifier_first == 'c') {
            consputc(va_arg(arguments, uint));
        } else if (specifier_first == 's') {
            if ((string = va_arg(arguments, char *)) == 0)
                string = "(null)";
            for (; *string; string++)
                consputc(*string);
        } else if (specifier_first == '%') {
            consputc('%');
        } else if (specifier_first == 0) {
            break;
        } else {
            // 未知の%シーケンスを出力して注意を促す。
            consputc('%');
            consputc(specifier_first);
        }
    }
    va_end(arguments);

    if (panicking == 0)
        release(&print_state.lock);

    return 0;
}

void panic(char *message)
{
    panicking = 1;
    printk("panic: ");
    printk("%s\n", message);
    panicked = 1; // 他のCPUからのUART出力を停止する
    for (;;)
        ;
}

void printkinit(void) { initlock(&print_state.lock, "print_state"); }
