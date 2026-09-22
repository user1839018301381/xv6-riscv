#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buffer[512];

void wc(int fd, char *name)
{
    int i, bytes_read;
    int line_count, word_count, byte_count, is_in_word;

    line_count = word_count = byte_count = 0;
    is_in_word = 0;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        for (i = 0; i < bytes_read; i++) {
            byte_count++;
            if (buffer[i] == '\n')
                line_count++;
            if (strchr(" \r\t\n\v", buffer[i]))
                is_in_word = 0;
            else if (!is_in_word) {
                word_count++;
                is_in_word = 1;
            }
        }
    }
    if (bytes_read < 0) {
        printf("wc: read error\n");
        exit(1);
    }
    printf("%d %d %d %s\n", line_count, word_count, byte_count, name);
}

int main(int argc, char *argv[])
{
    int fd, i;

    if (argc <= 1) {
        wc(0, "");
        exit(0);
    }

    for (i = 1; i < argc; i++) {
        if ((fd = open(argv[i], O_RDONLY)) < 0) {
            printf("wc: cannot open %s\n", argv[i]);
            exit(1);
        }
        wc(fd, argv[i]);
        close(fd);
    }
    exit(0);
}
