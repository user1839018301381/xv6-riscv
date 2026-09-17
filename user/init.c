// init: 最初のユーザレベルプログラム

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = {"sh", 0};

int main(void)
{
    int pid, wpid;

    if (open("console", O_RDWR) < 0) {
        mknod("console", CONSOLE, 0);
        open("console", O_RDWR);
    }
    dup(0); // 標準出力
    dup(0); // 標準エラー出力

    for (;;) {
        printf("init: starting sh\n");
        pid = fork();
        if (pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }
        if (pid == 0) {
            exec("sh", argv);
            printf("init: exec sh failed\n");
            exit(1);
        }

        for (;;) {
            // この wait() はシェルが終了した場合や、
            // 親を持たないプロセスが終了した場合に復帰する。
            wpid = wait((int *)0);
            if (wpid == pid) {
                // シェルが終了したので再起動する。
                break;
            } else if (wpid < 0) {
                printf("init: wait returned an error\n");
                exit(1);
            } else {
                // 親を持たないプロセスだったので何もしない。
            }
        }
    }
}
