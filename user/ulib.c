#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "kernel/vm.h"
#include "user/user.h"

//
// main()がexit()を呼ばなくてもよいようにするラッパー。
//
void start(int argc, char **argv)
{
    int exit_status;
    extern int main(int argc, char **argv);
    exit_status = main(argc, argv);
    exit(exit_status);
}

char *strcpy(char *destination, const char *source)
{
    char *original_destination;

    original_destination = destination;
    while ((*destination++ = *source++) != 0)
        ;
    return original_destination;
}

int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right)
        left++, right++;
    return (uchar)*left - (uchar)*right;
}

uint strlen(const char *string)
{
    int length;

    for (length = 0; string[length]; length++)
        ;
    return length;
}

void *memset(void *destination, int value, uint byte_count)
{
    char *destination_bytes = (char *)destination;
    int i;
    for (i = 0; i < byte_count; i++) {
        destination_bytes[i] = value;
    }
    return destination;
}

char *strchr(const char *string, char character)
{
    for (; *string; string++)
        if (*string == character)
            return (char *)string;
    return 0;
}

char *gets(char *buffer, int max_length)
{
    int i, bytes_read;
    char character;

    for (i = 0; i + 1 < max_length;) {
        bytes_read = read(0, &character, 1);
        if (bytes_read < 1)
            break;
        buffer[i++] = character;
        if (character == '\n' || character == '\r')
            break;
    }
    buffer[i] = '\0';
    return buffer;
}

int stat(const char *path, struct stat *file_status)
{
    int fd;
    int status;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    status = fstat(fd, file_status);
    close(fd);
    return status;
}

int atoi(const char *string)
{
    int value;

    value = 0;
    while ('0' <= *string && *string <= '9')
        value = value * 10 + *string++ - '0';
    return value;
}

void *memmove(void *destination, const void *source, int byte_count)
{
    char *destination_bytes;
    const char *source_bytes;

    destination_bytes = destination;
    source_bytes = source;
    if (source_bytes > destination_bytes) {
        while (byte_count-- > 0)
            *destination_bytes++ = *source_bytes++;
    } else {
        destination_bytes += byte_count;
        source_bytes += byte_count;
        while (byte_count-- > 0)
            *--destination_bytes = *--source_bytes;
    }
    return destination;
}

int memcmp(const void *left, const void *right, uint byte_count)
{
    const char *left_bytes = left, *right_bytes = right;
    while (byte_count-- > 0) {
        if (*left_bytes != *right_bytes) {
            return *left_bytes - *right_bytes;
        }
        left_bytes++;
        right_bytes++;
    }
    return 0;
}

void *memcpy(void *destination, const void *source, uint byte_count)
{
    return memmove(destination, source, byte_count);
}

char *sbrk(int byte_count) { return sys_sbrk(byte_count, SBRK_EAGER); }

char *sbrklazy(int byte_count) { return sys_sbrk(byte_count, SBRK_LAZY); }
