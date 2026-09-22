#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

static void putc(int fd, char character) { write(fd, &character, 1); }

static void printint(int fd, long long value, int base, int is_signed)
{
    char digit_buffer[20];
    int digit_index, is_negative;
    unsigned long long magnitude;

    is_negative = 0;
    if (is_signed && value < 0) {
        is_negative = 1;
        magnitude = -value;
    } else {
        magnitude = value;
    }

    digit_index = 0;
    do {
        digit_buffer[digit_index++] = digits[magnitude % base];
    } while ((magnitude /= base) != 0);
    if (is_negative)
        digit_buffer[digit_index++] = '-';

    while (--digit_index >= 0)
        putc(fd, digit_buffer[digit_index]);
}

static void printptr(int fd, uint64 pointer_value)
{
    int i;
    putc(fd, '0');
    putc(fd, 'x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, pointer_value <<= 4)
        putc(fd, digits[pointer_value >> (sizeof(uint64) * 8 - 4)]);
}

// 指定の fd へ出力する。%d、%x、%p、%c、%s のみ解釈する。
void vprintf(int fd, const char *format, va_list arguments)
{
    char *string;
    int specifier_first, specifier_second, specifier_third, i;
    int is_parsing_specifier;

    is_parsing_specifier = 0;
    for (i = 0; format[i]; i++) {
        specifier_first = format[i] & 0xff;
        if (!is_parsing_specifier) {
            if (specifier_first == '%') {
                is_parsing_specifier = 1;
            } else {
                putc(fd, specifier_first);
            }
        } else {
            specifier_second = specifier_third = 0;
            if (specifier_first)
                specifier_second = format[i + 1] & 0xff;
            if (specifier_second)
                specifier_third = format[i + 2] & 0xff;
            if (specifier_first == 'd') {
                printint(fd, va_arg(arguments, int), 10, 1);
            } else if (specifier_first == 'l' && specifier_second == 'd') {
                printint(fd, va_arg(arguments, uint64), 10, 1);
                i += 1;
            } else if (specifier_first == 'l' && specifier_second == 'l' &&
                       specifier_third == 'd') {
                printint(fd, va_arg(arguments, uint64), 10, 1);
                i += 2;
            } else if (specifier_first == 'u') {
                printint(fd, va_arg(arguments, uint32), 10, 0);
            } else if (specifier_first == 'l' && specifier_second == 'u') {
                printint(fd, va_arg(arguments, uint64), 10, 0);
                i += 1;
            } else if (specifier_first == 'l' && specifier_second == 'l' &&
                       specifier_third == 'u') {
                printint(fd, va_arg(arguments, uint64), 10, 0);
                i += 2;
            } else if (specifier_first == 'x') {
                printint(fd, va_arg(arguments, uint32), 16, 0);
            } else if (specifier_first == 'l' && specifier_second == 'x') {
                printint(fd, va_arg(arguments, uint64), 16, 0);
                i += 1;
            } else if (specifier_first == 'l' && specifier_second == 'l' &&
                       specifier_third == 'x') {
                printint(fd, va_arg(arguments, uint64), 16, 0);
                i += 2;
            } else if (specifier_first == 'p') {
                printptr(fd, va_arg(arguments, uint64));
            } else if (specifier_first == 'c') {
                putc(fd, va_arg(arguments, uint32));
            } else if (specifier_first == 's') {
                if ((string = va_arg(arguments, char *)) == 0)
                    string = "(null)";
                for (; *string; string++)
                    putc(fd, *string);
            } else if (specifier_first == '%') {
                putc(fd, '%');
            } else {
                // 未知の%シーケンス。注意を促すためそのまま出力する。
                putc(fd, '%');
                putc(fd, specifier_first);
            }

            is_parsing_specifier = 0;
        }
    }
}

void fprintf(int fd, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(fd, format, arguments);
}

void printf(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(1, format, arguments);
}
