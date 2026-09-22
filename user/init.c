// init: 最初のユーザレベルプログラム

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *shell_argv[] = {"sh", 0};

int main(void)
{
    int shell_pid, exited_pid;

    if (open("console", O_RDWR) < 0) {
        mknod("console", CONSOLE, 0);
        open("console", O_RDWR);
    }
    dup(0); // 標準出力
    dup(0); // 標準エラー出力

    for (;;) {
        printf("init: starting sh\n");
        shell_pid = fork();
        if (shell_pid < 0) {
            printf("init: fork failed\n");
            exit(1);
        }
        if (shell_pid == 0) {
            exec("sh", shell_argv);
            printf("init: exec sh failed\n");
            exit(1);
        }

        for (;;) {
            // この wait() はシェルが終了した場合や、
            // 親を持たないプロセスが終了した場合に復帰する。
            exited_pid = wait((int *)0);
            if (exited_pid == shell_pid) {
                // シェルが終了したので再起動する。
                break;
            } else if (exited_pid < 0) {
                printf("init: wait returned an error\n");
                exit(1);
            } else {
                // 親を持たないプロセスだったので何もしない。
            }
        }
    }
}
