// 単純な grep。^ . * $ 演算子のみ対応する。

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buffer[1024];
int match(char *expression, char *text);

void grep(char *pattern, int fd)
{
    int bytes_read, buffered_byte_count;
    char *line_start, *newline;

    buffered_byte_count = 0;
    while ((bytes_read = read(fd, buffer + buffered_byte_count,
                              sizeof(buffer) - buffered_byte_count - 1)) > 0) {
        buffered_byte_count += bytes_read;
        buffer[buffered_byte_count] = '\0';
        line_start = buffer;
        while ((newline = strchr(line_start, '\n')) != 0) {
            *newline = 0;
            if (match(pattern, line_start)) {
                *newline = '\n';
                write(1, line_start, newline + 1 - line_start);
            }
            line_start = newline + 1;
        }
        if (buffered_byte_count > 0) {
            buffered_byte_count -= line_start - buffer;
            memmove(buffer, line_start, buffered_byte_count);
        }
    }
}

int main(int argc, char *argv[])
{
    int fd, i;
    char *pattern;

    if (argc <= 1) {
        fprintf(2, "usage: grep pattern [file ...]\n");
        exit(1);
    }
    pattern = argv[1];

    if (argc <= 2) {
        grep(pattern, 0);
        exit(0);
    }

    for (i = 2; i < argc; i++) {
        if ((fd = open(argv[i], O_RDONLY)) < 0) {
            printf("grep: cannot open %s\n", argv[i]);
            exit(1);
        }
        grep(pattern, fd);
        close(fd);
    }
    exit(0);
}

// Kernighan & Pike 著『The Practice of Programming』第9章由来の正規表現マッチャー、
// または下記 URL の解説を参照。
// https://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html

int matchhere(char *expression, char *text);
int matchstar(int repeated_character, char *expression, char *text);

int match(char *expression, char *text)
{
    if (expression[0] == '^')
        return matchhere(expression + 1, text);
    do { // 空文字列も調べる必要がある
        if (matchhere(expression, text))
            return 1;
    } while (*text++ != '\0');
    return 0;
}

// matchhere: テキスト先頭で正規表現 re を探す
int matchhere(char *expression, char *text)
{
    if (expression[0] == '\0')
        return 1;
    if (expression[1] == '*')
        return matchstar(expression[0], expression + 2, text);
    if (expression[0] == '$' && expression[1] == '\0')
        return *text == '\0';
    if (*text != '\0' &&
        (expression[0] == '.' || expression[0] == *text))
        return matchhere(expression + 1, text + 1);
    return 0;
}

// matchstar: テキスト先頭で c*re を探す
int matchstar(int repeated_character, char *expression, char *text)
{
    do { // a* は0回以上の繰り返しに一致する
        if (matchhere(expression, text))
            return 1;
    } while (*text != '\0' &&
             (*text++ == repeated_character || repeated_character == '.'));
    return 0;
}
