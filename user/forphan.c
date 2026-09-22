#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// 孤児になったファイルを作成し、test-xv6.py が回収できるか確認する。

#define BUFSZ 500

char buf[BUFSZ];

int main(int argc, char **argv)
{
    int fd = 0;
    char *program_name = argv[0];
    struct stat file_status;
    char *file_name = "file0";

    if ((fd = open(file_name, O_CREATE | O_WRONLY)) < 0) {
        printf("%s: open failed\n", program_name);
        exit(1);
    }
    if (fstat(fd, &file_status) < 0) {
        fprintf(2, "%s: cannot stat %s\n", program_name, file_name);
        exit(1);
    }
    if (unlink(file_name) < 0) {
        printf("%s: unlink failed\n", program_name);
        exit(1);
    }
    if (open(file_name, O_RDONLY) != -1) {
        printf("%s: open successed\n", program_name);
        exit(1);
    }
    printf("wait for kill and reclaim %d\n", file_status.inode_number);
    // 終了させられるまで待機する
    for (;;)
        pause(1000);
}
