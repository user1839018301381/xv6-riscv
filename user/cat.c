#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buffer[512];

void cat(int fd)
{
    int bytes_read;

    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        if (write(1, buffer, bytes_read) != bytes_read) {
            fprintf(2, "cat: write error\n");
            exit(1);
        }
    }
    if (bytes_read < 0) {
        fprintf(2, "cat: read error\n");
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    int fd, i;

    if (argc <= 1) {
        cat(0);
        exit(0);
    }

    for (i = 1; i < argc; i++) {
        if ((fd = open(argv[i], O_RDONLY)) < 0) {
            fprintf(2, "cat: cannot open %s\n", argv[i]);
            exit(1);
        }
        cat(fd);
        close(fd);
    }
    exit(0);
}
