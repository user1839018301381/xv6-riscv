// fork が正常に失敗することをテストする。
// プロセス表を埋め尽くすことが上限になるよう、小さな実行ファイルにしている。

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N 1000

void print(const char *message) { write(1, message, strlen(message)); }

void forktest(void)
{
    int child_count, pid;

    print("fork test\n");

    for (child_count = 0; child_count < N; child_count++) {
        pid = fork();
        if (pid < 0)
            break;
        if (pid == 0)
            exit(0);
    }

    if (child_count == N) {
        print("fork claimed to work N times!\n");
        exit(1);
    }

    for (; child_count > 0; child_count--) {
        if (wait(0) < 0) {
            print("wait stopped early\n");
            exit(1);
        }
    }

    if (wait(0) != -1) {
        print("wait got too many\n");
        exit(1);
    }

    print("fork test OK\n");
}

int main(void)
{
    forktest();
    exit(0);
}
