#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// 孤児になったディレクトリを作成し、test-xv6.py が回収できるか確認する。

#define BUFSZ 500

char buf[BUFSZ];

int main(int argc, char **argv)
{
    char *s = argv[0];

    if (mkdir("dd") != 0) {
        printf("%s: mkdir dd failed\n", s);
        exit(1);
    }

    if (chdir("dd") != 0) {
        printf("%s: chdir dd failed\n", s);
        exit(1);
    }

    if (unlink("../dd") < 0) {
        printf("%s: unlink failed\n", s);
        exit(1);
    }
    printf("wait for kill and reclaim\n");
    // 終了させられるまで待機する
    for (;;)
        pause(1000);
}
