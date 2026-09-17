// 終了時に親を付け替えなければならない
// ゾンビプロセスを作成する。

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(void)
{
    if (fork() > 0)
        pause(5); // 親より先に子が終了するよう待つ。
    exit(0);
}
