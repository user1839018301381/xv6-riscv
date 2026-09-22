// idequeueへ追加するループの後にiderw内の「acquire」を移すと
// 競合が発生することを示す。

// これを動かすにはiderwのidequeue走査ループ内にもスピンを追加する。
// 次の処理を追加すると、2.1GHz CPUのQEMUでstressfsを約5回実行した後に
// panicが発生した:
//    for (i = 0; i < 40000; i++)
//      asm volatile("");

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

int main(int argc, char *argv[])
{
    int fd, i;
    char path[] = "stressfs0";
    char buffer[512];

    printf("stressfs starting\n");
    memset(buffer, 'a', sizeof(buffer));

    for (i = 0; i < 4; i++)
        if (fork() > 0)
            break;

    printf("write %d\n", i);

    path[8] += i;
    fd = open(path, O_CREATE | O_RDWR);
    for (i = 0; i < 20; i++) {
        // printf(fd, "%d\n", i);
        write(fd, buffer, sizeof(buffer));
    }
    close(fd);

    printf("read\n");

    fd = open(path, O_RDONLY);
    for (i = 0; i < 20; i++)
        read(fd, buffer, sizeof(buffer));
    close(fd);

    wait(0);

    exit(0);
}
