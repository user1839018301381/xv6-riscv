#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

//
// xv6のシステムコールをテストする。引数なしのusertestsは全テストを実行し、
// usertests <name>は<name>テストを実行する。テストランナーは各テスト用に
// プロセスを作り、その終了ステータスに基づいて"OK"または"FAILED"を報告する。
// テストによってはカーネルがusertrapメッセージを表示するが、テストが"OK"を
// 表示したなら無視できる。
//

#define BUFSZ ((MAXOPBLOCKS + 2) * BSIZE)

char buffer[BUFSZ];

//
// 比較的速く実行できるテストのセクション。これらだけ実行するには-qを使う。
// -qなしでは、かなり時間のかかるテストもusertestsが実行する。
//

// copyinでユーザメモリを読むシステムコールに、
// 異常なポインタを渡したらどうなるか?
void copyin(char *test_name)
{
    uint64 invalid_addresses[] = {0x80000000LL, 0x3fffffe000, 0x3ffffff000,
                                  0x4000000000, 0xffffffffffffffff};

    for (int address_index = 0;
         address_index < sizeof(invalid_addresses) /
                             sizeof(invalid_addresses[0]);
         address_index++) {
        uint64 address = invalid_addresses[address_index];

        int fd = open("copyin1", O_CREATE | O_WRONLY);
        if (fd < 0) {
            printf("open(copyin1) failed\n");
            exit(1);
        }
        int io_result = write(fd, (void *)address, 8192);
        if (io_result >= 0) {
            printf("write(fd, %p, 8192) returned %d, not -1\n",
                   (void *)address, io_result);
            exit(1);
        }
        close(fd);
        unlink("copyin1");

        io_result = write(1, (char *)address, 8192);
        if (io_result > 0) {
            printf("write(1, %p, 8192) returned %d, not -1 or 0\n",
                   (void *)address, io_result);
            exit(1);
        }

        int pipe_fds[2];
        if (pipe(pipe_fds) < 0) {
            printf("pipe() failed\n");
            exit(1);
        }
        io_result = write(pipe_fds[1], (char *)address, 8192);
        if (io_result > 0) {
            printf("write(pipe, %p, 8192) returned %d, not -1 or 0\n",
                   (void *)address, io_result);
            exit(1);
        }
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }
}

// copyoutでユーザメモリへ書くシステムコールに、
// 異常なポインタを渡したらどうなるか?
void copyout(char *test_name)
{
    uint64 invalid_addresses[] = {0LL,          0x80000000LL, 0x3fffffe000,
                                  0x3ffffff000, 0x4000000000,
                                  0xffffffffffffffff};

    for (int address_index = 0;
         address_index < sizeof(invalid_addresses) /
                             sizeof(invalid_addresses[0]);
         address_index++) {
        uint64 address = invalid_addresses[address_index];

        int fd = open("README", 0);
        if (fd < 0) {
            printf("open(README) failed\n");
            exit(1);
        }
        int io_result = read(fd, (void *)address, 8192);
        if (io_result > 0) {
            printf("read(fd, %p, 8192) returned %d, not -1 or 0\n",
                   (void *)address, io_result);
            exit(1);
        }
        close(fd);

        int pipe_fds[2];
        if (pipe(pipe_fds) < 0) {
            printf("pipe() failed\n");
            exit(1);
        }
        io_result = write(pipe_fds[1], "x", 1);
        if (io_result != 1) {
            printf("pipe write failed\n");
            exit(1);
        }
        io_result = read(pipe_fds[0], (void *)address, 8192);
        if (io_result > 0) {
            printf("read(pipe, %p, 8192) returned %d, not -1 or 0\n",
                   (void *)address, io_result);
            exit(1);
        }
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }
}

// システムコールに異常な文字列ポインタを渡したらどうなるか?
void copyinstr1(char *test_name)
{
    uint64 invalid_addresses[] = {0x80000000LL, 0x3fffffe000, 0x3ffffff000,
                                  0x4000000000, 0xffffffffffffffff};

    for (int address_index = 0;
         address_index < sizeof(invalid_addresses) /
                             sizeof(invalid_addresses[0]);
         address_index++) {
        uint64 address = invalid_addresses[address_index];

        int fd = open((char *)address, O_CREATE | O_WRONLY);
        if (fd >= 0) {
            printf("open(%p) returned %d, not -1\n", (void *)address, fd);
            exit(1);
        }
    }
}

// 文字列システムコール引数が複写先のカーネルバッファとちょうど同じサイズで、
// NULがカーネルバッファの末尾のすぐ外側に来たらどうなるか?
void copyinstr2(char *test_name)
{
    char overlong_path[MAXPATH + 1];

    for (int i = 0; i < MAXPATH; i++)
        overlong_path[i] = 'x';
    overlong_path[MAXPATH] = '\0';

    int syscall_result = unlink(overlong_path);
    if (syscall_result != -1) {
        printf("unlink(%s) returned %d, not -1\n", overlong_path,
               syscall_result);
        exit(1);
    }

    int fd = open(overlong_path, O_CREATE | O_WRONLY);
    if (fd != -1) {
        printf("open(%s) returned %d, not -1\n", overlong_path, fd);
        exit(1);
    }

    syscall_result = link(overlong_path, overlong_path);
    if (syscall_result != -1) {
        printf("link(%s, %s) returned %d, not -1\n", overlong_path,
               overlong_path, syscall_result);
        exit(1);
    }

    char *arguments[] = {"xx", 0};
    syscall_result = exec(overlong_path, arguments);
    if (syscall_result != -1) {
        printf("exec(%s) returned %d, not -1\n", overlong_path, fd);
        exit(1);
    }

    int pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        static char big[PGSIZE + 1];
        for (int i = 0; i < PGSIZE; i++)
            big[i] = 'x';
        big[PGSIZE] = '\0';
        char *large_arguments[] = {big, big, big, 0};
        syscall_result = exec("echo", large_arguments);
        if (syscall_result != -1) {
            printf("exec(echo, BIG) returned %d, not -1\n", fd);
            exit(1);
        }
        exit(747); // OK
    }

    int exit_status = 0;
    wait(&exit_status);
    if (exit_status != 747) {
        printf("exec(echo, BIG) succeeded, should have failed\n");
        exit(1);
    }
}

// 文字列引数が最後のユーザページの境界を越えたらどうなるか?
void copyinstr3(char *test_name)
{
    sbrk(8192);
    uint64 top = (uint64)sbrk(0);
    if ((top % PGSIZE) != 0) {
        sbrk(PGSIZE - (top % PGSIZE));
    }
    top = (uint64)sbrk(0);
    if (top % PGSIZE) {
        printf("oops\n");
        exit(1);
    }

    char *unterminated_path = (char *)(top - 1);
    *unterminated_path = 'x';

    int syscall_result = unlink(unterminated_path);
    if (syscall_result != -1) {
        printf("unlink(%s) returned %d, not -1\n", unterminated_path,
               syscall_result);
        exit(1);
    }

    int fd = open(unterminated_path, O_CREATE | O_WRONLY);
    if (fd != -1) {
        printf("open(%s) returned %d, not -1\n", unterminated_path, fd);
        exit(1);
    }

    syscall_result = link(unterminated_path, unterminated_path);
    if (syscall_result != -1) {
        printf("link(%s, %s) returned %d, not -1\n", unterminated_path,
               unterminated_path, syscall_result);
        exit(1);
    }

    char *arguments[] = {"xx", 0};
    syscall_result = exec(unterminated_path, arguments);
    if (syscall_result != -1) {
        printf("exec(%s) returned %d, not -1\n", unterminated_path, fd);
        exit(1);
    }
}

// アプリケーションが返却したため、もはや所有していないユーザメモリを
// カーネルが読み書き拒否することを確認する。
void rwsbrk(char *test_name)
{
    int fd, io_result;

    uint64 allocation = (uint64)sbrk(8192);

    if (allocation == (uint64)SBRK_ERROR) {
        printf("sbrk(rwsbrk) failed\n");
        exit(1);
    }

    if (sbrk(-8192) == SBRK_ERROR) {
        printf("sbrk(rwsbrk) shrink failed\n");
        exit(1);
    }

    fd = open("rwsbrk", O_CREATE | O_WRONLY);
    if (fd < 0) {
        printf("open(rwsbrk) failed\n");
        exit(1);
    }
    io_result = write(fd, (void *)(allocation + PGSIZE), 1024);
    if (io_result >= 0) {
        printf("write(fd, %p, 1024) returned %d, not -1\n",
               (void *)allocation + PGSIZE, io_result);
        exit(1);
    }
    close(fd);
    unlink("rwsbrk");

    fd = open("README", O_RDONLY);
    if (fd < 0) {
        printf("open(README) failed\n");
        exit(1);
    }
    io_result = read(fd, (void *)(allocation + PGSIZE), 10);
    if (io_result >= 0) {
        printf("read(fd, %p, 10) returned %d, not -1\n",
               (void *)allocation + PGSIZE, io_result);
        exit(1);
    }
    close(fd);

    exit(0);
}

// O_TRUNCをテストする。
void truncate1(char *test_name)
{
    char buffer[32];

    unlink("truncfile");
    int fd1 = open("truncfile", O_CREATE | O_WRONLY | O_TRUNC);
    write(fd1, "abcd", 4);
    close(fd1);

    int fd2 = open("truncfile", O_RDONLY);
    int bytes_read = read(fd2, buffer, sizeof(buffer));
    if (bytes_read != 4) {
        printf("%s: read %d bytes, wanted 4\n", test_name, bytes_read);
        exit(1);
    }

    fd1 = open("truncfile", O_WRONLY | O_TRUNC);

    int fd3 = open("truncfile", O_RDONLY);
    bytes_read = read(fd3, buffer, sizeof(buffer));
    if (bytes_read != 0) {
        printf("aaa fd3=%d\n", fd3);
        printf("%s: read %d bytes, wanted 0\n", test_name, bytes_read);
        exit(1);
    }

    bytes_read = read(fd2, buffer, sizeof(buffer));
    if (bytes_read != 0) {
        printf("bbb fd2=%d\n", fd2);
        printf("%s: read %d bytes, wanted 0\n", test_name, bytes_read);
        exit(1);
    }

    write(fd1, "abcdef", 6);

    bytes_read = read(fd3, buffer, sizeof(buffer));
    if (bytes_read != 6) {
        printf("%s: read %d bytes, wanted 6\n", test_name, bytes_read);
        exit(1);
    }

    bytes_read = read(fd2, buffer, sizeof(buffer));
    if (bytes_read != 2) {
        printf("%s: read %d bytes, wanted 2\n", test_name, bytes_read);
        exit(1);
    }

    unlink("truncfile");

    close(fd1);
    close(fd2);
    close(fd3);
}

// 直前にファイルが切り詰められたオープン中のFDへ書き込む。
// これによりファイル末尾を越えたオフセットへの書き込みが発生する。
// このような書き込みはxv6では（POSIXとは異なり）失敗するが、少なくともクラッシュしない。
void truncate2(char *test_name)
{
    unlink("truncfile");

    int fd1 = open("truncfile", O_CREATE | O_TRUNC | O_WRONLY);
    write(fd1, "abcd", 4);

    int fd2 = open("truncfile", O_TRUNC | O_WRONLY);

    int bytes_written = write(fd1, "x", 1);
    if (bytes_written != -1) {
        printf("%s: write returned %d, expected -1\n", test_name,
               bytes_written);
        exit(1);
    }

    unlink("truncfile");
    close(fd1);
    close(fd2);
}

void truncate3(char *test_name)
{
    int pid, exit_status;

    close(open("truncfile", O_CREATE | O_TRUNC | O_WRONLY));

    pid = fork();
    if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }

    if (pid == 0) {
        for (int i = 0; i < 100; i++) {
            char buffer[32];
            int fd = open("truncfile", O_WRONLY);
            if (fd < 0) {
                printf("%s: open failed\n", test_name);
                exit(1);
            }
            int bytes_written = write(fd, "1234567890", 10);
            if (bytes_written != 10) {
                printf("%s: write got %d, expected 10\n", test_name,
                       bytes_written);
                exit(1);
            }
            close(fd);
            fd = open("truncfile", O_RDONLY);
            read(fd, buffer, sizeof(buffer));
            close(fd);
        }
        exit(0);
    }

    for (int i = 0; i < 150; i++) {
        int fd = open("truncfile", O_CREATE | O_WRONLY | O_TRUNC);
        if (fd < 0) {
            printf("%s: open failed\n", test_name);
            exit(1);
        }
        int bytes_written = write(fd, "xxx", 3);
        if (bytes_written != 3) {
            printf("%s: write got %d, expected 3\n", test_name,
                   bytes_written);
            exit(1);
        }
        close(fd);
    }

    wait(&exit_status);
    unlink("truncfile");
    exit(exit_status);
}

// chdir()はトランザクション内でiput(p->cwd)を呼ぶか?
void iputtest(char *test_name)
{
    if (mkdir("iputdir") < 0) {
        printf("%s: mkdir failed\n", test_name);
        exit(1);
    }
    if (chdir("iputdir") < 0) {
        printf("%s: chdir iputdir failed\n", test_name);
        exit(1);
    }
    if (unlink("../iputdir") < 0) {
        printf("%s: unlink ../iputdir failed\n", test_name);
        exit(1);
    }
    if (chdir("/") < 0) {
        printf("%s: chdir / failed\n", test_name);
        exit(1);
    }
}

// exit()はトランザクション内でiput(p->cwd)を呼ぶか?
void exitiputtest(char *test_name)
{
    int pid, exit_status;

    pid = fork();
    if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    if (pid == 0) {
        if (mkdir("iputdir") < 0) {
            printf("%s: mkdir failed\n", test_name);
            exit(1);
        }
        if (chdir("iputdir") < 0) {
            printf("%s: child chdir failed\n", test_name);
            exit(1);
        }
        if (unlink("../iputdir") < 0) {
            printf("%s: unlink ../iputdir failed\n", test_name);
            exit(1);
        }
        exit(0);
    }
    wait(&exit_status);
    exit(exit_status);
}

// ディレクトリへの書き込みを試みるopen()のエラーパスは、
// トランザクション内でiput()を呼ぶか?
// sys_open()内のnamei()呼び出し直後に停止する改造カーネルが必要:
//    if((ip = namei(path)) == 0)
//      return -1;
//    {
//      int i;
//      for(i = 0; i < 10000; i++)
//        yield();
//    }
void openiputtest(char *test_name)
{
    int pid, exit_status;

    if (mkdir("oidir") < 0) {
        printf("%s: mkdir oidir failed\n", test_name);
        exit(1);
    }
    pid = fork();
    if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    if (pid == 0) {
        int fd = open("oidir", O_RDWR);
        if (fd >= 0) {
            printf("%s: open directory for write succeeded\n", test_name);
            exit(1);
        }
        exit(0);
    }
    pause(1);
    if (unlink("oidir") != 0) {
        printf("%s: unlink failed\n", test_name);
        exit(1);
    }
    wait(&exit_status);
    exit(exit_status);
}

// 単純なファイルシステムテスト

void opentest(char *test_name)
{
    int fd;

    fd = open("echo", 0);
    if (fd < 0) {
        printf("%s: open echo failed!\n", test_name);
        exit(1);
    }
    close(fd);
    fd = open("doesnotexist", 0);
    if (fd >= 0) {
        printf("%s: open doesnotexist succeeded!\n", test_name);
        exit(1);
    }
}

void writetest(char *test_name)
{
    int fd;
    int i, bytes_read;
    enum { N = 100, SZ = 10 };

    fd = open("small", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: error: creat small failed!\n", test_name);
        exit(1);
    }
    for (i = 0; i < N; i++) {
        if (write(fd, "aaaaaaaaaa", SZ) != SZ) {
            printf("%s: error: write aa %d new file failed\n", test_name, i);
            exit(1);
        }
        if (write(fd, "bbbbbbbbbb", SZ) != SZ) {
            printf("%s: error: write bb %d new file failed\n", test_name, i);
            exit(1);
        }
    }
    close(fd);
    fd = open("small", O_RDONLY);
    if (fd < 0) {
        printf("%s: error: open small failed!\n", test_name);
        exit(1);
    }
    bytes_read = read(fd, buffer, N * SZ * 2);
    if (bytes_read != N * SZ * 2) {
        printf("%s: read failed\n", test_name);
        exit(1);
    }
    close(fd);

    if (unlink("small") < 0) {
        printf("%s: unlink small failed\n", test_name);
        exit(1);
    }
}

void writebig(char *test_name)
{
    int i, fd, block_count;

    fd = open("big", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: error: creat big failed!\n", test_name);
        exit(1);
    }

    for (i = 0; i < MAXFILE; i++) {
        ((int *)buffer)[0] = i;
        if (write(fd, buffer, BSIZE) != BSIZE) {
            printf("%s: error: write big file failed i=%d\n", test_name, i);
            exit(1);
        }
    }

    close(fd);

    fd = open("big", O_RDONLY);
    if (fd < 0) {
        printf("%s: error: open big failed!\n", test_name);
        exit(1);
    }

    block_count = 0;
    for (;;) {
        int bytes_read = read(fd, buffer, BSIZE);
        if (bytes_read == 0) {
            if (block_count != MAXFILE) {
                printf("%s: read only %d blocks from big", test_name,
                       block_count);
                exit(1);
            }
            break;
        } else if (bytes_read != BSIZE) {
            printf("%s: read failed %d\n", test_name, bytes_read);
            exit(1);
        }
        if (((int *)buffer)[0] != block_count) {
            printf("%s: read content of block %d is %d\n", test_name,
                   block_count, ((int *)buffer)[0]);
            exit(1);
        }
        block_count++;
    }
    close(fd);
    if (unlink("big") < 0) {
        printf("%s: unlink big failed\n", test_name);
        exit(1);
    }
}

// 多数のcreateの後にunlinkをテストする
void createtest(char *test_name)
{
    int i, fd;
    enum { N = 52 };

    char name[3];
    name[0] = 'a';
    name[2] = '\0';
    for (i = 0; i < N; i++) {
        name[1] = '0' + i;
        fd = open(name, O_CREATE | O_RDWR);
        close(fd);
    }
    name[0] = 'a';
    name[2] = '\0';
    for (i = 0; i < N; i++) {
        name[1] = '0' + i;
        unlink(name);
    }
}

void dirtest(char *test_name)
{
    if (mkdir("dir0") < 0) {
        printf("%s: mkdir failed\n", test_name);
        exit(1);
    }

    if (chdir("dir0") < 0) {
        printf("%s: chdir dir0 failed\n", test_name);
        exit(1);
    }

    if (chdir("..") < 0) {
        printf("%s: chdir .. failed\n", test_name);
        exit(1);
    }

    if (unlink("dir0") < 0) {
        printf("%s: unlink dir0 failed\n", test_name);
        exit(1);
    }
}

void exectest(char *test_name)
{
    int fd, exit_status, pid;
    char *echoargv[] = {"echo", "OK", 0};
    char buffer[3];

    unlink("echo-ok");
    pid = fork();
    if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    if (pid == 0) {
        int errfd = dup(1);
        if (errfd < 0) {
            printf("%s: dup failed\n", test_name);
            exit(1);
        }
        close(1);
        fd = open("echo-ok", O_CREATE | O_WRONLY);
        if (fd < 0) {
            fprintf(errfd, "%s: create failed\n", test_name);
            exit(1);
        }
        if (fd != 1) {
            fprintf(errfd, "%s: wrong fd\n", test_name);
            exit(1);
        }
        if (exec("echo", echoargv) < 0) {
            fprintf(errfd, "%s: exec echo failed\n", test_name);
            exit(1);
        }
        // ここには到達しない
    }
    if (wait(&exit_status) != pid) {
        printf("%s: wait failed!\n", test_name);
    }
    if (exit_status != 0) {
        printf("%s: nonzero wait status %d\n", test_name, exit_status);
        exit(1);
    }

    fd = open("echo-ok", O_RDONLY);
    if (fd < 0) {
        printf("%s: open failed\n", test_name);
        exit(1);
    }
    if (read(fd, buffer, 2) != 2) {
        printf("%s: read failed\n", test_name);
        exit(1);
    }
    unlink("echo-ok");
    if (buffer[0] == 'O' && buffer[1] == 'K')
        exit(0);
    else {
        printf("%s: wrong output\n", test_name);
        exit(1);
    }
}

// 単純なforkとパイプの読み書き

void pipe1(char *test_name)
{
    int pipe_fds[2], pid, exit_status;
    int sequence_number, i, chunk_index, bytes_read, requested_size;
    int total_bytes;
    enum { N = 5, SZ = 1033 };

    if (pipe(pipe_fds) != 0) {
        printf("%s: pipe() failed\n", test_name);
        exit(1);
    }
    pid = fork();
    sequence_number = 0;
    if (pid == 0) {
        close(pipe_fds[0]);
        for (chunk_index = 0; chunk_index < N; chunk_index++) {
            for (i = 0; i < SZ; i++)
                buffer[i] = sequence_number++;
            if (write(pipe_fds[1], buffer, SZ) != SZ) {
                printf("%s: pipe1 oops 1\n", test_name);
                exit(1);
            }
        }
        exit(0);
    } else if (pid > 0) {
        close(pipe_fds[1]);
        total_bytes = 0;
        requested_size = 1;
        while ((bytes_read =
                    read(pipe_fds[0], buffer, requested_size)) > 0) {
            for (i = 0; i < bytes_read; i++) {
                if ((buffer[i] & 0xff) !=
                    (sequence_number++ & 0xff)) {
                    printf("%s: pipe1 oops 2\n", test_name);
                    return;
                }
            }
            total_bytes += bytes_read;
            requested_size = requested_size * 2;
            if (requested_size > sizeof(buffer))
                requested_size = sizeof(buffer);
        }
        if (total_bytes != N * SZ) {
            printf("%s: pipe1 oops 3 total %d\n", test_name, total_bytes);
            exit(1);
        }
        close(pipe_fds[0]);
        wait(&exit_status);
        exit(exit_status);
    } else {
        printf("%s: fork() failed\n", test_name);
        exit(1);
    }
}

// 子が終了対象になるかテストする（ステータス=-1）
void killstatus(char *test_name)
{
    int exit_status;

    for (int i = 0; i < 100; i++) {
        int pid1 = fork();
        if (pid1 < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (pid1 == 0) {
            while (1) {
                getpid();
            }
            exit(0);
        }
        pause(1);
        kill(pid1);
        wait(&exit_status);
        if (exit_status != -1) {
            printf("%s: status should be -1\n", test_name);
            exit(1);
        }
    }
    exit(0);
}

// 最大2CPUで実行することを想定
void preempt(char *test_name)
{
    int pid1, pid2, pid3;
    int pipe_fds[2];

    pid1 = fork();
    if (pid1 < 0) {
        printf("%s: fork failed", test_name);
        exit(1);
    }
    if (pid1 == 0)
        for (;;)
            ;

    pid2 = fork();
    if (pid2 < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    if (pid2 == 0)
        for (;;)
            ;

    pipe(pipe_fds);
    pid3 = fork();
    if (pid3 < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    if (pid3 == 0) {
        close(pipe_fds[0]);
        if (write(pipe_fds[1], "x", 1) != 1)
            printf("%s: preempt write error", test_name);
        close(pipe_fds[1]);
        for (;;)
            ;
    }

    close(pipe_fds[1]);
    if (read(pipe_fds[0], buffer, sizeof(buffer)) != 1) {
        printf("%s: preempt read error", test_name);
        return;
    }
    close(pipe_fds[0]);
    printf("kill... ");
    kill(pid1);
    kill(pid2);
    kill(pid3);
    printf("wait... ");
    wait(0);
    wait(0);
    wait(0);
}

// exitとwaitの間の競合を探す
void exitwait(char *test_name)
{
    int i, pid;

    for (i = 0; i < 100; i++) {
        pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (pid) {
            int xstate;
            if (wait(&xstate) != pid) {
                printf("%s: wait wrong pid\n", test_name);
                exit(1);
            }
            if (i != xstate) {
                printf("%s: wait wrong exit status\n", test_name);
                exit(1);
            }
        } else {
            exit(i);
        }
    }
}

// 子が生きている親の終了を処理するreparentingコードで
// 競合を探す。
void reparent(char *test_name)
{
    int master_pid = getpid();
    for (int i = 0; i < 200; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (pid) {
            if (wait(0) != pid) {
                printf("%s: wait wrong pid\n", test_name);
                exit(1);
            }
        } else {
            int pid2 = fork();
            if (pid2 < 0) {
                kill(master_pid);
                exit(1);
            }
            exit(0);
        }
    }
    exit(0);
}

// 2つの子が同時にexit()したらどうなるか?
void twochildren(char *test_name)
{
    for (int i = 0; i < 1000; i++) {
        int pid1 = fork();
        if (pid1 < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (pid1 == 0) {
            exit(0);
        } else {
            int pid2 = fork();
            if (pid2 < 0) {
                printf("%s: fork failed\n", test_name);
                exit(1);
            }
            if (pid2 == 0) {
                exit(0);
            } else {
                wait(0);
                wait(0);
            }
        }
    }
}

// 並行forkでロックのバグを見つけようとする。
void forkfork(char *test_name)
{
    enum { N = 2 };

    for (int i = 0; i < N; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("%s: fork failed", test_name);
            exit(1);
        }
        if (pid == 0) {
            for (int j = 0; j < 200; j++) {
                int pid1 = fork();
                if (pid1 < 0) {
                    exit(1);
                }
                if (pid1 == 0) {
                    exit(0);
                }
                wait(0);
            }
            exit(0);
        }
    }

    int exit_status;
    for (int i = 0; i < N; i++) {
        wait(&exit_status);
        if (exit_status != 0) {
            printf("%s: fork in child failed", test_name);
            exit(1);
        }
    }
}

void forkforkfork(char *test_name)
{
    unlink("stopforking");

    int pid = fork();
    if (pid < 0) {
        printf("%s: fork failed", test_name);
        exit(1);
    }
    if (pid == 0) {
        while (1) {
            int fd = open("stopforking", 0);
            if (fd >= 0) {
                exit(0);
            }
            if (fork() < 0) {
                close(open("stopforking", O_CREATE | O_RDWR));
            }
        }

        exit(0);
    }

    pause(20); // 2秒
    close(open("stopforking", O_CREATE | O_RDWR));
    wait(0);
    pause(10); // 1秒
}

// 回帰テスト。reparent()が子をinitへ渡すとき、親、子の順の
// ロック順序に違反してexit()とinitのwait()がデッドロックしないか?
// exit()が取得したものとは別のp->parent->lockを解放することで起きる
// "panic: release"の再現にも使う。
void reparent2(char *test_name)
{
    for (int i = 0; i < 800; i++) {
        int pid1 = fork();
        if (pid1 < 0) {
            printf("fork failed\n");
            exit(1);
        }
        if (pid1 == 0) {
            fork();
            fork();
            exit(0);
        }
        wait(0);
    }

    exit(0);
}

// 全メモリを割り当て、解放し、再び割り当てる
void mem(char *test_name)
{
    void *allocation_list, *allocation;
    int pid;

    if ((pid = fork()) == 0) {
        allocation_list = 0;
        while ((allocation = malloc(10001)) != 0) {
            *(char **)allocation = allocation_list;
            allocation_list = allocation;
        }
        while (allocation_list) {
            allocation = *(char **)allocation_list;
            free(allocation_list);
            allocation_list = allocation;
        }
        allocation_list = malloc(1024 * 20);
        if (allocation_list == 0) {
            printf("%s: couldn't allocate mem?!!\n", test_name);
            exit(1);
        }
        free(allocation_list);
        exit(0);
    } else {
        int exit_status;
        wait(&exit_status);
        if (exit_status == -1) {
            // おそらくページフォルトなので、遅延割り当ての課題かもしれず、
            // 問題ない。
            exit(0);
        }
        exit(exit_status);
    }
}

// さらにファイルシステムテスト

// 2つのプロセスが同じファイルディスクリプタへ書き込む。
// オフセットは共有されるか? inodeロックは機能するか?
void sharedfd(char *test_name)
{
    int fd, pid, i, bytes_read, child_byte_count, parent_byte_count;
    enum { N = 1000, SZ = 10 };
    char buffer[SZ];

    unlink("sharedfd");
    fd = open("sharedfd", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: cannot open sharedfd for writing", test_name);
        exit(1);
    }
    pid = fork();
    memset(buffer, pid == 0 ? 'c' : 'p', sizeof(buffer));
    for (i = 0; i < N; i++) {
        if (write(fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
            printf("%s: write sharedfd failed\n", test_name);
            exit(1);
        }
    }
    if (pid == 0) {
        exit(0);
    } else {
        int exit_status;
        wait(&exit_status);
        if (exit_status != 0)
            exit(exit_status);
    }

    close(fd);
    fd = open("sharedfd", 0);
    if (fd < 0) {
        printf("%s: cannot open sharedfd for reading\n", test_name);
        exit(1);
    }
    child_byte_count = parent_byte_count = 0;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        for (i = 0; i < sizeof(buffer); i++) {
            if (buffer[i] == 'c')
                child_byte_count++;
            if (buffer[i] == 'p')
                parent_byte_count++;
        }
    }
    close(fd);
    unlink("sharedfd");
    if (child_byte_count == N * SZ && parent_byte_count == N * SZ) {
        exit(0);
    } else {
        printf("%s: nc/np test fails\n", test_name);
        exit(1);
    }
}

// ブロック割り当てをテストするため、4つのプロセスが
// 同時に異なるファイルへ書き込む。
void fourfiles(char *test_name)
{
    int fd, pid, i, j, io_result, total_bytes, process_index;
    char *names[] = {"f0", "f1", "f2", "f3"};
    char *file_name;
    enum { N = 12, NCHILD = 4, SZ = 500 };

    for (process_index = 0; process_index < NCHILD; process_index++) {
        file_name = names[process_index];
        unlink(file_name);

        pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }

        if (pid == 0) {
            fd = open(file_name, O_CREATE | O_RDWR);
            if (fd < 0) {
                printf("%s: create failed\n", test_name);
                exit(1);
            }

            memset(buffer, '0' + process_index, SZ);
            for (i = 0; i < N; i++) {
                if ((io_result = write(fd, buffer, SZ)) != SZ) {
                    printf("write failed %d\n", io_result);
                    exit(1);
                }
            }
            exit(0);
        }
    }

    int exit_status;
    for (process_index = 0; process_index < NCHILD; process_index++) {
        wait(&exit_status);
        if (exit_status != 0)
            exit(exit_status);
    }

    for (i = 0; i < NCHILD; i++) {
        file_name = names[i];
        fd = open(file_name, 0);
        total_bytes = 0;
        while ((io_result = read(fd, buffer, sizeof(buffer))) > 0) {
            for (j = 0; j < io_result; j++) {
                if (buffer[j] != '0' + i) {
                    printf("%s: wrong char\n", test_name);
                    exit(1);
                }
            }
            total_bytes += io_result;
        }
        close(fd);
        if (total_bytes != N * SZ) {
            printf("wrong length %d\n", total_bytes);
            exit(1);
        }
        unlink(file_name);
    }
}

// 4つのプロセスが同じディレクトリ内で異なるファイルを作成・削除する
void createdelete(char *test_name)
{
    enum { N = 20, NCHILD = 4 };
    int pid, i, fd, process_index;
    char name[32];

    for (process_index = 0; process_index < NCHILD; process_index++) {
        pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }

        if (pid == 0) {
            name[0] = 'p' + process_index;
            name[2] = '\0';
            for (i = 0; i < N; i++) {
                name[1] = '0' + i;
                fd = open(name, O_CREATE | O_RDWR);
                if (fd < 0) {
                    printf("%s: create failed\n", test_name);
                    exit(1);
                }
                close(fd);
                if (i > 0 && (i % 2) == 0) {
                    name[1] = '0' + (i / 2);
                    if (unlink(name) < 0) {
                        printf("%s: unlink failed\n", test_name);
                        exit(1);
                    }
                }
            }
            exit(0);
        }
    }

    int exit_status;
    for (process_index = 0; process_index < NCHILD; process_index++) {
        wait(&exit_status);
        if (exit_status != 0)
            exit(1);
    }

    name[0] = name[1] = name[2] = 0;
    for (i = 0; i < N; i++) {
        for (process_index = 0; process_index < NCHILD; process_index++) {
            name[0] = 'p' + process_index;
            name[1] = '0' + i;
            fd = open(name, 0);
            if ((i == 0 || i >= N / 2) && fd < 0) {
                printf("%s: oops createdelete %s didn't exist\n", test_name, name);
                exit(1);
            } else if ((i >= 1 && i < N / 2) && fd >= 0) {
                printf("%s: oops createdelete %s did exist\n", test_name, name);
                exit(1);
            }
            if (fd >= 0)
                close(fd);
        }
    }

    for (i = 0; i < N; i++) {
        for (process_index = 0; process_index < NCHILD; process_index++) {
            name[0] = 'p' + process_index;
            name[1] = '0' + i;
            unlink(name);
        }
    }
}

// ファイルのリンクを解除しても読み込めるか?
void unlinkread(char *test_name)
{
    enum { SZ = 5 };
    int fd, replacement_fd;

    fd = open("unlinkread", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: create unlinkread failed\n", test_name);
        exit(1);
    }
    write(fd, "hello", SZ);
    close(fd);

    fd = open("unlinkread", O_RDWR);
    if (fd < 0) {
        printf("%s: open unlinkread failed\n", test_name);
        exit(1);
    }
    if (unlink("unlinkread") != 0) {
        printf("%s: unlink unlinkread failed\n", test_name);
        exit(1);
    }

    replacement_fd = open("unlinkread", O_CREATE | O_RDWR);
    write(replacement_fd, "yyy", 3);
    close(replacement_fd);

    if (read(fd, buffer, sizeof(buffer)) != SZ) {
        printf("%s: unlinkread read failed", test_name);
        exit(1);
    }
    if (buffer[0] != 'h') {
        printf("%s: unlinkread wrong data\n", test_name);
        exit(1);
    }
    if (write(fd, buffer, 10) != 10) {
        printf("%s: unlinkread write failed\n", test_name);
        exit(1);
    }
    close(fd);
    unlink("unlinkread");
}

void linktest(char *test_name)
{
    enum { SZ = 5 };
    int fd;

    unlink("lf1");
    unlink("lf2");

    fd = open("lf1", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: create lf1 failed\n", test_name);
        exit(1);
    }
    if (write(fd, "hello", SZ) != SZ) {
        printf("%s: write lf1 failed\n", test_name);
        exit(1);
    }
    close(fd);

    if (link("lf1", "lf2") < 0) {
        printf("%s: link lf1 lf2 failed\n", test_name);
        exit(1);
    }
    unlink("lf1");

    if (open("lf1", 0) >= 0) {
        printf("%s: unlinked lf1 but it is still there!\n", test_name);
        exit(1);
    }

    fd = open("lf2", 0);
    if (fd < 0) {
        printf("%s: open lf2 failed\n", test_name);
        exit(1);
    }
    if (read(fd, buffer, sizeof(buffer)) != SZ) {
        printf("%s: read lf2 failed\n", test_name);
        exit(1);
    }
    close(fd);

    if (link("lf2", "lf2") >= 0) {
        printf("%s: link lf2 lf2 succeeded! oops\n", test_name);
        exit(1);
    }

    unlink("lf2");
    if (link("lf2", "lf1") >= 0) {
        printf("%s: link non-existent succeeded! oops\n", test_name);
        exit(1);
    }

    if (link(".", "lf1") >= 0) {
        printf("%s: link . lf1 succeeded! oops\n", test_name);
        exit(1);
    }
}

// 同じファイルに対するcreate/link/unlinkの並行実行をテストする
void concreate(char *test_name)
{
    enum { N = 40 };
    char file[3];
    int i, pid, matching_file_count, fd;
    char found_files[N];
    struct {
        ushort inode_number;
        char name[DIRSIZ];
    } directory_entry;

    file[0] = 'C';
    file[2] = '\0';
    for (i = 0; i < N; i++) {
        file[1] = '0' + i;
        unlink(file);
        pid = fork();
        if (pid && (i % 3) == 1) {
            link("C0", file);
        } else if (pid == 0 && (i % 5) == 1) {
            link("C0", file);
        } else {
            fd = open(file, O_CREATE | O_RDWR);
            if (fd < 0) {
                printf("concreate create %s failed\n", file);
                exit(1);
            }
            close(fd);
        }
        if (pid == 0) {
            exit(0);
        } else {
            int exit_status;
            wait(&exit_status);
            if (exit_status != 0)
                exit(1);
        }
    }

    memset(found_files, 0, sizeof(found_files));
    fd = open(".", 0);
    matching_file_count = 0;
    while (read(fd, &directory_entry, sizeof(directory_entry)) > 0) {
        if (directory_entry.inode_number == 0)
            continue;
        if (directory_entry.name[0] == 'C' &&
            directory_entry.name[2] == '\0') {
            i = directory_entry.name[1] - '0';
            if (i < 0 || i >= sizeof(found_files)) {
                printf("%s: concreate weird file %s\n", test_name,
                       directory_entry.name);
                exit(1);
            }
            if (found_files[i]) {
                printf("%s: concreate duplicate file %s\n", test_name,
                       directory_entry.name);
                exit(1);
            }
            found_files[i] = 1;
            matching_file_count++;
        }
    }
    close(fd);

    if (matching_file_count != N) {
        printf("%s: concreate not enough files in directory listing\n", test_name);
        exit(1);
    }

    for (i = 0; i < N; i++) {
        file[1] = '0' + i;
        pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (((i % 3) == 0 && pid == 0) || ((i % 3) == 1 && pid != 0)) {
            close(open(file, 0));
            close(open(file, 0));
            close(open(file, 0));
            close(open(file, 0));
            close(open(file, 0));
            close(open(file, 0));
        } else {
            unlink(file);
            unlink(file);
            unlink(file);
            unlink(file);
            unlink(file);
            unlink(file);
        }
        if (pid == 0)
            exit(0);
        else
            wait(0);
    }
}

// デッドロックを探すための、別の並行link/unlink/createテスト。
void linkunlink(char *test_name)
{
    int pid, i;

    unlink("x");
    pid = fork();
    if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }

    unsigned int random_value = (pid ? 1 : 97);
    for (i = 0; i < 100; i++) {
        random_value = random_value * 1103515245 + 12345;
        if ((random_value % 3) == 0) {
            close(open("x", O_RDWR | O_CREATE));
        } else if ((random_value % 3) == 1) {
            link("cat", "x");
        } else {
            unlink("x");
        }
    }

    if (pid)
        wait(0);
    else
        exit(0);
}

void subdir(char *test_name)
{
    int fd, bytes_read;

    unlink("ff");
    if (mkdir("dd") != 0) {
        printf("%s: mkdir dd failed\n", test_name);
        exit(1);
    }

    fd = open("dd/ff", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: create dd/ff failed\n", test_name);
        exit(1);
    }
    write(fd, "ff", 2);
    close(fd);

    if (unlink("dd") >= 0) {
        printf("%s: unlink dd (non-empty dir) succeeded!\n", test_name);
        exit(1);
    }

    if (mkdir("/dd/dd") != 0) {
        printf("%s: subdir mkdir dd/dd failed\n", test_name);
        exit(1);
    }

    fd = open("dd/dd/ff", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: create dd/dd/ff failed\n", test_name);
        exit(1);
    }
    write(fd, "FF", 2);
    close(fd);

    fd = open("dd/dd/../ff", 0);
    if (fd < 0) {
        printf("%s: open dd/dd/../ff failed\n", test_name);
        exit(1);
    }
    bytes_read = read(fd, buffer, sizeof(buffer));
    if (bytes_read != 2 || buffer[0] != 'f') {
        printf("%s: dd/dd/../ff wrong content\n", test_name);
        exit(1);
    }
    close(fd);

    if (link("dd/dd/ff", "dd/dd/ffff") != 0) {
        printf("%s: link dd/dd/ff dd/dd/ffff failed\n", test_name);
        exit(1);
    }

    if (unlink("dd/dd/ff") != 0) {
        printf("%s: unlink dd/dd/ff failed\n", test_name);
        exit(1);
    }
    if (open("dd/dd/ff", O_RDONLY) >= 0) {
        printf("%s: open (unlinked) dd/dd/ff succeeded\n", test_name);
        exit(1);
    }

    if (chdir("dd") != 0) {
        printf("%s: chdir dd failed\n", test_name);
        exit(1);
    }
    if (chdir("dd/../../dd") != 0) {
        printf("%s: chdir dd/../../dd failed\n", test_name);
        exit(1);
    }
    if (chdir("dd/../../../dd") != 0) {
        printf("%s: chdir dd/../../../dd failed\n", test_name);
        exit(1);
    }
    if (chdir("./..") != 0) {
        printf("%s: chdir ./.. failed\n", test_name);
        exit(1);
    }

    fd = open("dd/dd/ffff", 0);
    if (fd < 0) {
        printf("%s: open dd/dd/ffff failed\n", test_name);
        exit(1);
    }
    if (read(fd, buffer, sizeof(buffer)) != 2) {
        printf("%s: read dd/dd/ffff wrong len\n", test_name);
        exit(1);
    }
    close(fd);

    if (open("dd/dd/ff", O_RDONLY) >= 0) {
        printf("%s: open (unlinked) dd/dd/ff succeeded!\n", test_name);
        exit(1);
    }

    if (open("dd/ff/ff", O_CREATE | O_RDWR) >= 0) {
        printf("%s: create dd/ff/ff succeeded!\n", test_name);
        exit(1);
    }
    if (open("dd/xx/ff", O_CREATE | O_RDWR) >= 0) {
        printf("%s: create dd/xx/ff succeeded!\n", test_name);
        exit(1);
    }
    if (open("dd", O_CREATE) >= 0) {
        printf("%s: create dd succeeded!\n", test_name);
        exit(1);
    }
    if (open("dd", O_RDWR) >= 0) {
        printf("%s: open dd rdwr succeeded!\n", test_name);
        exit(1);
    }
    if (open("dd", O_WRONLY) >= 0) {
        printf("%s: open dd wronly succeeded!\n", test_name);
        exit(1);
    }
    if (link("dd/ff/ff", "dd/dd/xx") == 0) {
        printf("%s: link dd/ff/ff dd/dd/xx succeeded!\n", test_name);
        exit(1);
    }
    if (link("dd/xx/ff", "dd/dd/xx") == 0) {
        printf("%s: link dd/xx/ff dd/dd/xx succeeded!\n", test_name);
        exit(1);
    }
    if (link("dd/ff", "dd/dd/ffff") == 0) {
        printf("%s: link dd/ff dd/dd/ffff succeeded!\n", test_name);
        exit(1);
    }
    if (mkdir("dd/ff/ff") == 0) {
        printf("%s: mkdir dd/ff/ff succeeded!\n", test_name);
        exit(1);
    }
    if (mkdir("dd/xx/ff") == 0) {
        printf("%s: mkdir dd/xx/ff succeeded!\n", test_name);
        exit(1);
    }
    if (mkdir("dd/dd/ffff") == 0) {
        printf("%s: mkdir dd/dd/ffff succeeded!\n", test_name);
        exit(1);
    }
    if (unlink("dd/xx/ff") == 0) {
        printf("%s: unlink dd/xx/ff succeeded!\n", test_name);
        exit(1);
    }
    if (unlink("dd/ff/ff") == 0) {
        printf("%s: unlink dd/ff/ff succeeded!\n", test_name);
        exit(1);
    }
    if (chdir("dd/ff") == 0) {
        printf("%s: chdir dd/ff succeeded!\n", test_name);
        exit(1);
    }
    if (chdir("dd/xx") == 0) {
        printf("%s: chdir dd/xx succeeded!\n", test_name);
        exit(1);
    }

    if (unlink("dd/dd/ffff") != 0) {
        printf("%s: unlink dd/dd/ff failed\n", test_name);
        exit(1);
    }
    if (unlink("dd/ff") != 0) {
        printf("%s: unlink dd/ff failed\n", test_name);
        exit(1);
    }
    if (unlink("dd") == 0) {
        printf("%s: unlink non-empty dd succeeded!\n", test_name);
        exit(1);
    }
    if (unlink("dd/dd") < 0) {
        printf("%s: unlink dd/dd failed\n", test_name);
        exit(1);
    }
    if (unlink("dd") < 0) {
        printf("%s: unlink dd failed\n", test_name);
        exit(1);
    }
}

// ログより大きい書き込みをテストする。
void bigwrite(char *test_name)
{
    int fd, write_size;

    unlink("bigwrite");
    for (write_size = 499; write_size < (MAXOPBLOCKS + 2) * BSIZE;
         write_size += 471) {
        fd = open("bigwrite", O_CREATE | O_RDWR);
        if (fd < 0) {
            printf("%s: cannot create bigwrite\n", test_name);
            exit(1);
        }
        int i;
        for (i = 0; i < 2; i++) {
            int bytes_written = write(fd, buffer, write_size);
            if (bytes_written != write_size) {
                printf("%s: write(%d) ret %d\n", test_name, write_size,
                       bytes_written);
                exit(1);
            }
        }
        close(fd);
        unlink("bigwrite");
    }
}

void bigfile(char *test_name)
{
    enum { N = 20, SZ = 600 };
    int fd, i, total_bytes, bytes_read;

    unlink("bigfile.dat");
    fd = open("bigfile.dat", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: cannot create bigfile", test_name);
        exit(1);
    }
    for (i = 0; i < N; i++) {
        memset(buffer, i, SZ);
        if (write(fd, buffer, SZ) != SZ) {
            printf("%s: write bigfile failed\n", test_name);
            exit(1);
        }
    }
    close(fd);

    fd = open("bigfile.dat", 0);
    if (fd < 0) {
        printf("%s: cannot open bigfile\n", test_name);
        exit(1);
    }
    total_bytes = 0;
    for (i = 0;; i++) {
        bytes_read = read(fd, buffer, SZ / 2);
        if (bytes_read < 0) {
            printf("%s: read bigfile failed\n", test_name);
            exit(1);
        }
        if (bytes_read == 0)
            break;
        if (bytes_read != SZ / 2) {
            printf("%s: short read bigfile\n", test_name);
            exit(1);
        }
        if (buffer[0] != i / 2 || buffer[SZ / 2 - 1] != i / 2) {
            printf("%s: read bigfile wrong data\n", test_name);
            exit(1);
        }
        total_bytes += bytes_read;
    }
    close(fd);
    if (total_bytes != N * SZ) {
        printf("%s: read bigfile wrong total\n", test_name);
        exit(1);
    }
    unlink("bigfile.dat");
}

void fourteen(char *test_name)
{
    int fd;

    // DIRSIZは14。

    if (mkdir("12345678901234") != 0) {
        printf("%s: mkdir 12345678901234 failed\n", test_name);
        exit(1);
    }
    if (mkdir("12345678901234/123456789012345") != 0) {
        printf("%s: mkdir 12345678901234/123456789012345 failed\n", test_name);
        exit(1);
    }
    fd = open("123456789012345/123456789012345/123456789012345", O_CREATE);
    if (fd < 0) {
        printf(
            "%s: create 123456789012345/123456789012345/123456789012345 failed\n",
            test_name);
        exit(1);
    }
    close(fd);
    fd = open("12345678901234/12345678901234/12345678901234", 0);
    if (fd < 0) {
        printf("%s: open 12345678901234/12345678901234/12345678901234 failed\n",
               test_name);
        exit(1);
    }
    close(fd);

    if (mkdir("12345678901234/12345678901234") == 0) {
        printf("%s: mkdir 12345678901234/12345678901234 succeeded!\n", test_name);
        exit(1);
    }
    if (mkdir("123456789012345/12345678901234") == 0) {
        printf("%s: mkdir 12345678901234/123456789012345 succeeded!\n", test_name);
        exit(1);
    }

    // 後片付け
    unlink("123456789012345/12345678901234");
    unlink("12345678901234/12345678901234");
    unlink("12345678901234/12345678901234/12345678901234");
    unlink("123456789012345/123456789012345/123456789012345");
    unlink("12345678901234/123456789012345");
    unlink("12345678901234");
}

void rmdot(char *test_name)
{
    if (mkdir("dots") != 0) {
        printf("%s: mkdir dots failed\n", test_name);
        exit(1);
    }
    if (chdir("dots") != 0) {
        printf("%s: chdir dots failed\n", test_name);
        exit(1);
    }
    if (unlink(".") == 0) {
        printf("%s: rm . worked!\n", test_name);
        exit(1);
    }
    if (unlink("..") == 0) {
        printf("%s: rm .. worked!\n", test_name);
        exit(1);
    }
    if (chdir("/") != 0) {
        printf("%s: chdir / failed\n", test_name);
        exit(1);
    }
    if (unlink("dots/.") == 0) {
        printf("%s: unlink dots/. worked!\n", test_name);
        exit(1);
    }
    if (unlink("dots/..") == 0) {
        printf("%s: unlink dots/.. worked!\n", test_name);
        exit(1);
    }
    if (unlink("dots") != 0) {
        printf("%s: unlink dots failed!\n", test_name);
        exit(1);
    }
}

void dirfile(char *test_name)
{
    int fd;

    fd = open("dirfile", O_CREATE);
    if (fd < 0) {
        printf("%s: create dirfile failed\n", test_name);
        exit(1);
    }
    close(fd);
    if (chdir("dirfile") == 0) {
        printf("%s: chdir dirfile succeeded!\n", test_name);
        exit(1);
    }
    fd = open("dirfile/xx", 0);
    if (fd >= 0) {
        printf("%s: create dirfile/xx succeeded!\n", test_name);
        exit(1);
    }
    fd = open("dirfile/xx", O_CREATE);
    if (fd >= 0) {
        printf("%s: create dirfile/xx succeeded!\n", test_name);
        exit(1);
    }
    if (mkdir("dirfile/xx") == 0) {
        printf("%s: mkdir dirfile/xx succeeded!\n", test_name);
        exit(1);
    }
    if (unlink("dirfile/xx") == 0) {
        printf("%s: unlink dirfile/xx succeeded!\n", test_name);
        exit(1);
    }
    if (link("README", "dirfile/xx") == 0) {
        printf("%s: link to dirfile/xx succeeded!\n", test_name);
        exit(1);
    }
    if (unlink("dirfile") != 0) {
        printf("%s: unlink dirfile failed!\n", test_name);
        exit(1);
    }

    fd = open(".", O_RDWR);
    if (fd >= 0) {
        printf("%s: open . for writing succeeded!\n", test_name);
        exit(1);
    }
    fd = open(".", 0);
    if (write(fd, "x", 1) > 0) {
        printf("%s: write . succeeded!\n", test_name);
        exit(1);
    }
    close(fd);
}

// _namei()の最後にiput()が呼ばれることをテストする。
// 空のファイル名もテストする。
void iref(char *test_name)
{
    int i, fd;

    for (i = 0; i < NINODE + 1; i++) {
        if (mkdir("irefd") != 0) {
            printf("%s: mkdir irefd failed\n", test_name);
            exit(1);
        }
        if (chdir("irefd") != 0) {
            printf("%s: chdir irefd failed\n", test_name);
            exit(1);
        }

        mkdir("");
        link("README", "");
        fd = open("", O_CREATE);
        if (fd >= 0)
            close(fd);
        fd = open("xx", O_CREATE);
        if (fd >= 0)
            close(fd);
        unlink("xx");
    }

    // 後片付け
    for (i = 0; i < NINODE + 1; i++) {
        chdir("..");
        unlink("irefd");
    }

    chdir("/");
}

// forkが正常に失敗することをテストする。
// forktestバイナリもこれを行うが、先にプロセス表を使い切る。
// より大きなusertestsバイナリ内では、先にメモリを使い切る。
void forktest(char *test_name)
{
    enum { N = 1000 };
    int child_count, pid;

    for (child_count = 0; child_count < N; child_count++) {
        pid = fork();
        if (pid < 0)
            break;
        if (pid == 0)
            exit(0);
    }

    if (child_count == 0) {
        printf("%s: no fork at all!\n", test_name);
        exit(1);
    }

    if (child_count == N) {
        printf("%s: fork claimed to work 1000 times!\n", test_name);
        exit(1);
    }

    for (; child_count > 0; child_count--) {
        if (wait(0) < 0) {
            printf("%s: wait stopped early\n", test_name);
            exit(1);
        }
    }

    if (wait(0) != -1) {
        printf("%s: wait got too many\n", test_name);
        exit(1);
    }
}

void sbrkbasic(char *test_name)
{
    enum { TOOMUCH = 1024 * 1024 * 1024 };
    int i, pid, exit_status;
    char *current_break, *expected_break, *allocation;

    // sbrk()は期待した失敗値を返すか?
    pid = fork();
    if (pid < 0) {
        printf("fork failed in sbrkbasic\n");
        exit(1);
    }
    if (pid == 0) {
        expected_break = sbrk(TOOMUCH);
        if (expected_break == (char *)SBRK_ERROR) {
            // これが失敗してもよい。
            exit(0);
        }

        for (allocation = expected_break;
             allocation < expected_break + TOOMUCH;
             allocation += PGSIZE) {
            *allocation = 99;
        }

        // ここには到達しないはず! sbrk(TOOMUCH)が失敗したか、
        // （遅延割り当てなら）ページフォルトでこのプロセスが
        // 終了させられているはずである。
        exit(1);
    }

    wait(&exit_status);
    if (exit_status == 1) {
        printf("%s: too much memory allocated!\n", test_name);
        exit(1);
    }

    // 1ページ未満のsbrk()は可能か?
    expected_break = sbrk(0);
    for (i = 0; i < 5000; i++) {
        allocation = sbrk(1);
        if (allocation != expected_break) {
            printf("%s: sbrk test failed %d %p %p\n", test_name, i,
                   expected_break, allocation);
            exit(1);
        }
        *allocation = 1;
        expected_break = allocation + 1;
    }
    pid = fork();
    if (pid < 0) {
        printf("%s: sbrk test fork failed\n", test_name);
        exit(1);
    }
    current_break = sbrk(1);
    current_break = sbrk(1);
    if (current_break != expected_break + 1) {
        printf("%s: sbrk test failed post-fork\n", test_name);
        exit(1);
    }
    if (pid == 0)
        exit(0);
    wait(&exit_status);
    exit(exit_status);
}

void sbrkmuch(char *test_name)
{
    enum { BIG = 100 * 1024 * 1024 };
    char *returned_break, *initial_break, *previous_break, *last_address;
    char *allocation_start;
    uint64 allocation_size;

    initial_break = sbrk(0);

    // アドレス空間を大きく拡張できるか?
    previous_break = sbrk(0);
    allocation_size = BIG - (uint64)previous_break;
    allocation_start = sbrk(allocation_size);
    if (allocation_start != previous_break) {
        printf(
            "%s: sbrk test failed to grow big address space; enough phys mem?\n",
            test_name);
        exit(1);
    }

    last_address = (char *)(BIG - 1);
    *last_address = 99;

    // 割り当てを解除できるか?
    previous_break = sbrk(0);
    returned_break = sbrk(-PGSIZE);
    if (returned_break == (char *)SBRK_ERROR) {
        printf("%s: sbrk could not deallocate\n", test_name);
        exit(1);
    }
    returned_break = sbrk(0);
    if (returned_break != previous_break - PGSIZE) {
        printf("%s: sbrk deallocation produced wrong address, a %p c %p\n",
               test_name, previous_break, returned_break);
        exit(1);
    }

    // そのページを再割り当てできるか?
    previous_break = sbrk(0);
    returned_break = sbrk(PGSIZE);
    if (returned_break != previous_break ||
        sbrk(0) != previous_break + PGSIZE) {
        printf("%s: sbrk re-allocation failed, a %p c %p\n", test_name,
               previous_break, returned_break);
        exit(1);
    }
    if (*last_address == 99) {
        // 0であるべき
        printf("%s: sbrk de-allocation didn't really deallocate\n", test_name);
        exit(1);
    }

    previous_break = sbrk(0);
    returned_break = sbrk(-(sbrk(0) - initial_break));
    if (returned_break != previous_break) {
        printf("%s: sbrk downsize failed, a %p c %p\n", test_name,
               previous_break, returned_break);
        exit(1);
    }
}

// カーネルのメモリを読み込めるか?
void kernmem(char *test_name)
{
    char *address;
    int pid;

    for (address = (char *)(KERNBASE);
         address < (char *)(KERNBASE + 2000000); address += 50000) {
        pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (pid == 0) {
            printf("%s: oops could read %p = %x\n", test_name, address,
                   *address);
            exit(1);
        }
        int exit_status;
        wait(&exit_status);
        if (exit_status != -1) // カーネルは子を終了させたか?
            exit(1);
    }
}

// ユーザコードはMAXVAより上のアドレスへ書けないはず。
void maxva_plus(char *test_name)
{
    volatile uint64 address = MAXVA;
    for (; address != 0; address <<= 1) {
        int pid;
        pid = fork();
        if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        if (pid == 0) {
            *(char *)address = 99;
            printf("%s: oops wrote %p\n", test_name, (void *)address);
            exit(1);
        }
        int exit_status;
        wait(&exit_status);
        if (exit_status != -1) // カーネルは子を終了させたか?
            exit(1);
    }
}

// システムのメモリを使い切ったとき、最後の失敗した割り当てを
// 後片付けするか?
void sbrkfail(char *test_name)
{
    enum { BIG = 100 * 1024 * 1024 };
    int i, exit_status;
    int pipe_fds[2];
    char scratch;
    char *recovered_page, *allocation;
    int child_pids[10];
    int pid;
    int allocation_failed;

    allocation_failed = 0;
    if (pipe(pipe_fds) != 0) {
        printf("%s: pipe() failed\n", test_name);
        exit(1);
    }
    for (i = 0; i < sizeof(child_pids) / sizeof(child_pids[0]); i++) {
        if ((child_pids[i] = fork()) == 0) {
            // 大量のメモリを割り当てる
            if (sbrk(BIG - (uint64)sbrk(0)) == (char *)SBRK_ERROR)
                write(pipe_fds[1], "0", 1);
            else
                write(pipe_fds[1], "1", 1);
            // 終了させられるまで待機する
            for (;;)
                pause(1000);
        }
        if (child_pids[i] != -1) {
            read(pipe_fds[0], &scratch, 1);
            if (scratch == '0')
                allocation_failed = 1;
        }
    }
    if (!allocation_failed) {
        printf("%s: no allocation failed; allocate more?\n", test_name);
    }

    // 失敗した割り当てが割り当て済みページを解放していれば、
    // ここで割り当てられるはず
    recovered_page = sbrk(PGSIZE);
    for (i = 0; i < sizeof(child_pids) / sizeof(child_pids[0]); i++) {
        if (child_pids[i] == -1)
            continue;
        kill(child_pids[i]);
        wait(0);
    }
    if (recovered_page == (char *)SBRK_ERROR) {
        printf("%s: failed sbrk leaked memory\n", test_name);
        exit(1);
    }

    // 上で割り当てたページがある状態でforkを実行するテスト
    pid = fork();
    if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    if (pid == 0) {
        // 大量のメモリを割り当てる。エラーになるはず
        allocation = sbrk(10 * BIG);
        if (allocation == (char *)SBRK_ERROR) {
            exit(0);
        }
        printf("%s: allocate a lot of memory succeeded %d\n", test_name, 10 * BIG);
        exit(1);
    }
    wait(&exit_status);
    if (exit_status != 0)
        exit(1);
}

// 割り当てたメモリの読み書きをテストする
void sbrkarg(char *test_name)
{
    char *allocation;
    int fd, bytes_written;

    allocation = sbrk(PGSIZE);
    fd = open("sbrk", O_CREATE | O_WRONLY);
    unlink("sbrk");
    if (fd < 0) {
        printf("%s: open sbrk failed\n", test_name);
        exit(1);
    }
    if ((bytes_written = write(fd, allocation, PGSIZE)) < 0) {
        printf("%s: write sbrk failed\n", test_name);
        exit(1);
    }
    close(fd);

    // 割り当てたメモリへの書き込みをテストする
    allocation = sbrk(PGSIZE);
    if (pipe((int *)allocation) != 0) {
        printf("%s: pipe() failed\n", test_name);
        exit(1);
    }
}

void validatetest(char *test_name)
{
    int highest_address;
    uint64 address;

    highest_address = 1100 * 1024;
    for (address = 0; address <= (uint)highest_address;
         address += PGSIZE) {
        // 不正な文字列ポインタを渡してカーネルをクラッシュさせようとする
        if (link("nosuchfile", (char *)address) != -1) {
            printf("%s: link should not succeed\n", test_name);
            exit(1);
        }
    }
}

// 初期化されていないデータは最初から0か?
char uninit[10000];
void bsstest(char *test_name)
{
    int i;

    for (i = 0; i < sizeof(uninit); i++) {
        if (uninit[i] != '\0') {
            printf("%s: bss test failed\n", test_name);
            exit(1);
        }
    }
}

// 引数が1ページより大きい場合、execはエラーを返すか?
// それともスタックより下へ書き込み、命令やデータを壊すか?
void bigargtest(char *test_name)
{
    int pid, fd, exit_status;

    unlink("bigarg-ok");
    pid = fork();
    if (pid == 0) {
        static char *args[MAXARG];
        int i;
        char big[400];
        memset(big, ' ', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        for (i = 0; i < MAXARG - 1; i++)
            args[i] = big;
        args[MAXARG - 1] = 0;
        // 引数が大きすぎるため、このexec()は失敗して戻るはず。
        exec("echo", args);
        fd = open("bigarg-ok", O_CREATE);
        close(fd);
        exit(0);
    } else if (pid < 0) {
        printf("%s: bigargtest: fork failed\n", test_name);
        exit(1);
    }

    wait(&exit_status);
    if (exit_status != 0)
        exit(exit_status);
    fd = open("bigarg-ok", 0);
    if (fd < 0) {
        printf("%s: bigarg test failed!\n", test_name);
        exit(1);
    }
    close(fd);
}

// ファイルシステムのブロックが尽きるとどうなるか?
// 答え: ballocがpanicするため、このテストは役に立たない。
void fsfull(void)
{
    int file_count;
    int file_system_block_count = 0;

    printf("fsfull test\n");

    for (file_count = 0;; file_count++) {
        char name[64];
        name[0] = 'f';
        name[1] = '0' + file_count / 1000;
        name[2] = '0' + (file_count % 1000) / 100;
        name[3] = '0' + (file_count % 100) / 10;
        name[4] = '0' + (file_count % 10);
        name[5] = '\0';
        printf("writing %s\n", name);
        int fd = open(name, O_CREATE | O_RDWR);
        if (fd < 0) {
            printf("open %s failed\n", name);
            break;
        }
        int total_bytes = 0;
        while (1) {
            int bytes_written = write(fd, buffer, BSIZE);
            if (bytes_written < BSIZE)
                break;
            total_bytes += bytes_written;
            file_system_block_count++;
        }
        printf("wrote %d bytes\n", total_bytes);
        close(fd);
        if (total_bytes == 0)
            break;
    }

    while (file_count >= 0) {
        char name[64];
        name[0] = 'f';
        name[1] = '0' + file_count / 1000;
        name[2] = '0' + (file_count % 1000) / 100;
        name[3] = '0' + (file_count % 100) / 10;
        name[4] = '0' + (file_count % 10);
        name[5] = '\0';
        unlink(name);
        file_count--;
    }

    printf("fsfull test finished, %d blocks\n", file_system_block_count);
}

void argptest(char *test_name)
{
    int fd;
    fd = open("init", O_RDONLY);
    if (fd < 0) {
        printf("%s: open failed\n", test_name);
        exit(1);
    }
    read(fd, sbrk(0) - 1, -1);
    close(fd);
}

// スタックオーバーフローを検出するため、ユーザスタックの下に
// 無効なページがあることを確認する。
void stacktest(char *test_name)
{
    int pid;
    int exit_status;

    pid = fork();
    if (pid == 0) {
        char *stack_pointer = (char *)r_sp();
        stack_pointer -= USERSTACK * PGSIZE;
        // *stack_pointerでトラップが発生するはず。
        printf("%s: stacktest: read below stack %d\n", test_name,
               *stack_pointer);
        exit(1);
    } else if (pid < 0) {
        printf("%s: fork failed\n", test_name);
        exit(1);
    }
    wait(&exit_status);
    if (exit_status == -1) // カーネルは子を終了させたか?
        exit(0);
    else
        exit(exit_status);
}

// いくつかの禁止アドレス（プロセスのテキストやTRAMPOLINEなど）への
// 書き込みでフォルトが発生することを確認する。
void nowrite(char *test_name)
{
    int pid;
    int exit_status;
    uint64 forbidden_addresses[] = {0,
                                    0x80000000LL,
                                    0x3fffffe000,
                                    0x3ffffff000,
                                    0x4000000000,
                                    0xffffffffffffffff};

    for (int address_index = 0;
         address_index < sizeof(forbidden_addresses) /
                             sizeof(forbidden_addresses[0]);
         address_index++) {
        pid = fork();
        if (pid == 0) {
            volatile int *address =
                (int *)forbidden_addresses[address_index];
            *address = 10;
            printf("%s: write to %p did not fail!\n", test_name, address);
            exit(0);
        } else if (pid < 0) {
            printf("%s: fork failed\n", test_name);
            exit(1);
        }
        wait(&exit_status);
        if (exit_status == 0) {
            // カーネルが子を終了させなかった!
            exit(1);
        }
    }
    exit(0);
}

// 回帰テスト。copyin()、copyout()、copyinstr()は以前、仮想ページアドレスを
// uintへキャストしていたため、（特定の異常なシステムコール引数で）
// カーネルページフォルトが発生していた。
void *big = (void *)0xeaeb0b5b00002f5e;
void pgbug(char *test_name)
{
    char *argv[1];
    argv[0] = 0;
    exec(big, argv);
    pipe(big);

    exit(0);
}

// 回帰テスト。プロセスがsbrk()でサイズを1ページ未満または0にしたり、
// ページ解放に足りない量だけブレークを減らしたりすると、カーネルはpanicするか?
void sbrkbugs(char *test_name)
{
    int pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        int process_size = (uint64)sbrk(0);
        // すべてのユーザメモリを解放する。この場合にp->szを正しく
        // 調整しないバグがあり、exit()がpanicしていた。
        sbrk(-process_size);
        // ここでユーザページフォルトが発生する。
        exit(0);
    }
    wait(0);

    pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        int process_size = (uint64)sbrk(0);
        // ブレークを最初のページ内に設定する。以前は最初のページを
        // 誤って解放するバグがあった。
        sbrk(-(process_size - 3500));
        exit(0);
    }
    wait(0);

    pid = fork();
    if (pid < 0) {
        printf("fork failed\n");
        exit(1);
    }
    if (pid == 0) {
        // ブレークをページの途中に設定する。
        sbrk((10 * PGSIZE + 2048) - (uint64)sbrk(0));

        // ブレークを少し減らすが、ページ解放が起きるほどではない。
        // 以前はこれでpanicが起きていた。
        sbrk(-10);

        exit(0);
    }
    wait(0);

    exit(0);
}

// プロセスサイズがページ境界を少し超えた後、そのページ境界を少し下回るまで
// 縮小されても、カーネルは最後のページのアドレスからcopyin()できるか?
void sbrklast(char *test_name)
{
    uint64 top = (uint64)sbrk(0);
    if ((top % PGSIZE) != 0)
        sbrk(PGSIZE - (top % PGSIZE));
    sbrk(PGSIZE);
    sbrk(10);
    sbrk(-20);
    top = (uint64)sbrk(0);
    char *path_buffer = (char *)(top - 64);
    path_buffer[0] = 'x';
    path_buffer[1] = '\0';
    int fd = open(path_buffer, O_RDWR | O_CREATE);
    write(fd, path_buffer, 1);
    close(fd);
    fd = open(path_buffer, O_RDWR);
    path_buffer[0] = '\0';
    read(fd, path_buffer, 1);
    if (path_buffer[0] != 'x')
        exit(1);
}

// sbrkは負の引数による符号付きint32のラップアラウンドを処理できるか?
void sbrk8000(char *test_name)
{
    sbrk(0x80000004);
    volatile char *top = sbrk(0);
    *(top - 1) = *(top - 1) + 1;
}

// 回帰テスト。引数の1つが無効な場合にexec()がメモリをリークするかテストする。
// カーネルがpanicしなければテストは成功である。
void badarg(char *test_name)
{
    for (int i = 0; i < 50000; i++) {
        char *argv[2];
        argv[0] = (char *)0xffffffff;
        argv[1] = 0;
        exec("echo", argv);
    }

    exit(0);
}

#define REGION_SZ (1024 * 1024 * 1024)

// 64ページごとに1ページへアクセスする。遅延割り当てでは
// 1ページが割り当てられる。
void lazy_alloc(char *test_name)
{
    char *cursor, *region_start, *region_end;

    region_start = sbrklazy(REGION_SZ);
    if (region_start == (char *)SBRK_ERROR) {
        printf("sbrklazy() failed\n");
        exit(1);
    }
    region_end = region_start + REGION_SZ;

    for (cursor = region_start + PGSIZE; cursor < region_end;
         cursor += 64 * PGSIZE)
        *(char **)cursor = cursor;

    for (cursor = region_start + PGSIZE; cursor < region_end;
         cursor += 64 * PGSIZE) {
        if (*(char **)cursor != cursor) {
            printf("failed to read value from memory\n");
            exit(1);
        }
    }

    exit(0);
}

// 領域内で64ページごとに1ページへアクセスする。遅延割り当てでは
// 1ページが割り当てられる。領域を解放すると割り当て済みページも
// 解放されることを確認する。
void lazy_unmap(char *test_name)
{
    int pid;
    char *cursor, *region_start, *region_end;

    region_start = sbrklazy(REGION_SZ);
    if (region_start == (char *)SBRK_ERROR) {
        printf("sbrklazy() failed\n");
        exit(1);
    }
    region_end = region_start + REGION_SZ;

    for (cursor = region_start + PGSIZE; cursor < region_end;
         cursor += PGSIZE * PGSIZE)
        *(char **)cursor = cursor;

    for (cursor = region_start + PGSIZE; cursor < region_end;
         cursor += PGSIZE * PGSIZE) {
        pid = fork();
        if (pid < 0) {
            printf("error forking\n");
            exit(1);
        } else if (pid == 0) {
            sbrklazy(-1L * REGION_SZ);
            *(char **)cursor = cursor;
            exit(0);
        } else {
            int exit_status;
            wait(&exit_status);
            if (exit_status == 0) {
                printf("memory not unmapped\n");
                exit(1);
            }
        }
    }

    exit(0);
}

void lazy_copy(char *test_name)
{
    // 遅延ページに対するcopyinstr
    {
        char *program_break = sbrk(0);
        sbrklazy(4 * PGSIZE);
        open(program_break + 8192, 0);
    }

    {
        void *previous_break = sbrk(0);
        void *returned_break = sbrk(-(((uint64)previous_break) + 1));
        if (returned_break != previous_break) {
            printf("sbrk(sbrk(0)+1) returned %p, not old sz\n",
                   returned_break);
            exit(1);
        }
    }

    // これらのアドレスへのread()とwrite()は失敗するはず。
    unsigned long invalid_addresses[] = {
        0x3fffffc000, 0x3fffffd000, 0x3fffffe000,
        0x3ffffff000, 0x4000000000, 0x8000000000,
    };
    for (int i = 0;
         i < sizeof(invalid_addresses) / sizeof(invalid_addresses[0]); i++) {
        int fd = open("README", 0);
        if (fd < 0) {
            printf("cannot open README\n");
            exit(1);
        }
        if (read(fd, (char *)invalid_addresses[i], 512) >= 0) {
            printf("read succeeded\n");
            exit(1);
        }
        close(fd);
        fd = open("junk", O_CREATE | O_RDWR | O_TRUNC);
        if (fd < 0) {
            printf("cannot open junk\n");
            exit(1);
        }
        if (write(fd, (char *)invalid_addresses[i], 512) >= 0) {
            printf("write succeeded\n");
            exit(1);
        }
        close(fd);
    }

    exit(0);
}

void lazy_copyinstr(char *test_name)
{
    char *path_buffer = sbrk(0);
    sbrk(PGSIZE - ((uint64)path_buffer % PGSIZE));

    path_buffer = sbrk(0);
    if ((uint64)path_buffer % PGSIZE != 0) {
        printf("%s: sbrk did not align\n", test_name);
        exit(1);
    }

    sbrklazy(2 * PGSIZE);
    path_buffer[4095] = '/';
    int fd = open(&path_buffer[4095], O_RDONLY);
    if (fd < 0) {
        printf("could not open /");
        exit(1);
    }

    struct stat file_status;
    int stat_result = fstat(fd, &file_status);
    if (stat_result < 0) {
        printf("could not stat /");
        exit(1);
    }

    if (file_status.type != T_DIR) {
        printf("/ is not T_DIR");
        exit(1);
    }

    close(fd);
}

void lazy_sbrk(char *test_name)
{
    // sbrk()はintだけを受け取るため、MAXVAへ向けて2^30ずつ進む
    char *current_break = sbrk(0);
    while ((uint64)current_break < MAXVA - (1 << 30)) {
        current_break = sbrklazy(1 << 30);
        if (current_break < 0) {
            printf("sbrklazy(%d) returned %p\n", 1 << 30, current_break);
            exit(1);
        }

        current_break = sbrklazy(0);
    }

    int growth = TRAPFRAME - PGSIZE - (uint64)current_break;

    char *returned_break = sbrklazy(growth);
    if (returned_break < 0 || returned_break != current_break) {
        printf("sbrklazy(%d) returned %p, not expected %p\n", growth,
               returned_break, current_break);
        exit(1);
    }

    current_break = sbrk(PGSIZE);
    if (current_break < 0 ||
        (uint64)current_break != TRAPFRAME - PGSIZE) {
        printf("sbrk(%d) returned %p, not expected TRAPFRAME-PGSIZE\n", PGSIZE,
               current_break);
        exit(1);
    }

    current_break[0] = 1;
    if (current_break[1] != 0) {
        printf("sbrk() returned non-zero-filled memory\n");
        exit(1);
    }

    current_break = sbrk(1);
    if ((uint64)current_break != -1) {
        printf("sbrk(1) returned %p, expected error\n", current_break);
        exit(1);
    }

    current_break = sbrklazy(1);
    if ((uint64)current_break != -1) {
        printf("sbrklazy(1) returned %p, expected error\n", current_break);
        exit(1);
    }

    exit(0);
}

void partial_write(char *test_name)
{
    // "A"を含むtestfileを作成する。
    // ページ境界をまたぐ2バイトをwrite()する。1バイト目は"X"、2バイト目は未マップ。
    // 起こりうる問題: writeエラーが更新済みの1バイト目をログに記録し忘れる。
    // ファイルからread()すると"X"が返る。
    // 大きな書き込みをいくつか行ってバッファキャッシュをフラッシュする。
    // ファイルからのread()でも"X"が返るはず（バグがあると"A"かもしれない）。

    unlink("testfile");
    int fd = open("testfile", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: cannot create testfile\n", test_name);
        exit(1);
    }

    int io_result = write(fd, "A", 1);
    if (io_result != 1) {
        printf("%s: could not write A\n", test_name);
        exit(1);
    }

    close(fd);
    fd = open("testfile", O_RDWR);
    if (fd < 0) {
        printf("%s: cannot re-open testfile\n", test_name);
        exit(1);
    }

    char *page_boundary = sbrk(0);
    sbrk(PGSIZE - ((uint64)page_boundary % PGSIZE));

    page_boundary = sbrk(0);
    if ((uint64)page_boundary % PGSIZE != 0) {
        printf("%s: sbrk did not align\n", test_name);
        exit(1);
    }

    page_boundary[-1] = 'X';

    io_result = write(fd, page_boundary - 1, 2);
    if (io_result != -1) {
        printf("%s: write succeeded, should have failed\n", test_name);
        exit(1);
    }

    close(fd);

    fd = open("testfile", O_RDONLY);
    if (fd < 0) {
        printf("%s: cannot re-open testfile\n", test_name);
        exit(1);
    }

    char byte;
    io_result = read(fd, &byte, 1);
    if (io_result != 1) {
        printf("%s: cannot read testfile\n", test_name);
        exit(1);
    }

    close(fd);

    if (byte != 'X') {
        printf("%s: read returned %c, expected X\n", test_name, byte);
        exit(1);
    }

    fd = open("bigfile", O_CREATE | O_RDWR);
    for (int i = 0; i < 64; i++) {
        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));
        io_result = write(fd, buffer, sizeof(buffer));
        if (io_result != sizeof(buffer)) {
            printf("%s: could not write to bigfile\n", test_name);
            exit(-1);
        }
    }
    close(fd);

    unlink("bigfile");

    fd = open("testfile", O_RDONLY);
    if (fd < 0) {
        printf("%s: cannot re-open testfile\n", test_name);
        exit(1);
    }

    io_result = read(fd, &byte, 1);
    if (io_result != 1) {
        printf("%s: cannot read testfile\n", test_name);
        exit(1);
    }

    close(fd);

    if (byte != 'X') {
        printf("%s: read returned %c, expected X\n", test_name, byte);
        exit(1);
    }

    unlink("testfile");
}

void unlinkcwd(char *test_name)
{
    if (mkdir("/a") < 0) {
        printf("%s: mkdir /a failed\n", test_name);
        exit(1);
    }
    if (mkdir("/a/b") < 0) {
        printf("%s: mkdir /a/b failed\n", test_name);
        exit(1);
    }
    if (chdir("/a/b") < 0) {
        printf("%s: chdir failed\n", test_name);
        exit(1);
    }
    if (unlink("/a/b") < 0) {
        printf("%s: unlink /a/b failed\n", test_name);
        exit(1);
    }
    if (unlink("/a") < 0) {
        printf("%s: unlink /a failed\n", test_name);
        exit(1);
    }
    if (open("../", O_RDONLY) > 0) {
        printf("%s: open ../ non-existing directory\n", test_name);
    }
    if (open("../c", O_CREATE) > 0) {
        printf("%s: create ../c non-existing file\n", test_name);
    }
}

struct test {
    void (*function)(char *);
    char *name;
} quick_tests[] = {
    {copyin, "copyin"},
    {copyout, "copyout"},
    {copyinstr1, "copyinstr1"},
    {copyinstr2, "copyinstr2"},
    {copyinstr3, "copyinstr3"},
    {rwsbrk, "rwsbrk"},
    {truncate1, "truncate1"},
    {truncate2, "truncate2"},
    {truncate3, "truncate3"},
    {openiputtest, "openiput"},
    {exitiputtest, "exitiput"},
    {iputtest, "iput"},
    {opentest, "opentest"},
    {writetest, "writetest"},
    {writebig, "writebig"},
    {createtest, "createtest"},
    {dirtest, "dirtest"},
    {exectest, "exectest"},
    {pipe1, "pipe1"},
    {killstatus, "killstatus"},
    {preempt, "preempt"},
    {exitwait, "exitwait"},
    {reparent, "reparent"},
    {twochildren, "twochildren"},
    {forkfork, "forkfork"},
    {forkforkfork, "forkforkfork"},
    {reparent2, "reparent2"},
    {mem, "mem"},
    {sharedfd, "sharedfd"},
    {fourfiles, "fourfiles"},
    {createdelete, "createdelete"},
    {unlinkread, "unlinkread"},
    {linktest, "linktest"},
    {concreate, "concreate"},
    {linkunlink, "linkunlink"},
    {subdir, "subdir"},
    {bigwrite, "bigwrite"},
    {bigfile, "bigfile"},
    {fourteen, "fourteen"},
    {rmdot, "rmdot"},
    {dirfile, "dirfile"},
    {iref, "iref"},
    {forktest, "forktest"},
    {sbrkbasic, "sbrkbasic"},
    {sbrkmuch, "sbrkmuch"},
    {kernmem, "kernmem"},
    {maxva_plus, "maxva_plus"},
    {sbrkfail, "sbrkfail"},
    {sbrkarg, "sbrkarg"},
    {validatetest, "validatetest"},
    {bsstest, "bsstest"},
    {bigargtest, "bigargtest"},
    {argptest, "argptest"},
    {stacktest, "stacktest"},
    {nowrite, "nowrite"},
    {pgbug, "pgbug"},
    {sbrkbugs, "sbrkbugs"},
    {sbrklast, "sbrklast"},
    {sbrk8000, "sbrk8000"},
    {badarg, "badarg"},
    {lazy_alloc, "lazy_alloc"},
    {lazy_unmap, "lazy_unmap"},
    {lazy_copy, "lazy_copy"},
    {lazy_copyinstr, "lazy_copyinstr"},
    {lazy_sbrk, "lazy_sbrk"},
    {partial_write, "partial_write"},
    {unlinkcwd, "unlinkcwd"},
    {0, 0},
};

//
// かなり時間のかかるテストのセクション
//

// 間接ブロックを使うディレクトリ
void bigdir(char *test_name)
{
    enum { N = 500 };
    int i, fd;
    char name[10];

    unlink("bd");

    fd = open("bd", O_CREATE);
    if (fd < 0) {
        printf("%s: bigdir create failed\n", test_name);
        exit(1);
    }
    close(fd);

    for (i = 0; i < N; i++) {
        name[0] = 'x';
        name[1] = '0' + (i / 64);
        name[2] = '0' + (i % 64);
        name[3] = '\0';
        if (link("bd", name) != 0) {
            printf("%s: bigdir i=%d link(bd, %s) failed\n", test_name, i, name);
            exit(1);
        }
    }

    unlink("bd");
    for (i = 0; i < N; i++) {
        name[0] = 'x';
        name[1] = '0' + (i / 64);
        name[2] = '0' + (i % 64);
        name[3] = '\0';
        if (unlink(name) != 0) {
            printf("%s: bigdir unlink failed", test_name);
            exit(1);
        }
    }
}

// virtioディスクドライバのデッドロックを誘発するための並行書き込み。
void manywrites(char *test_name)
{
    int child_count = 4;
    int iteration_count = 30; // デッドロックを探すには増やす

    for (int child_index = 0; child_index < child_count; child_index++) {
        int pid = fork();
        if (pid < 0) {
            printf("fork failed\n");
            exit(1);
        }

        if (pid == 0) {
            char name[3];
            name[0] = 'b';
            name[1] = 'a' + child_index;
            name[2] = '\0';
            unlink(name);

            for (int iteration = 0; iteration < iteration_count;
                 iteration++) {
                for (int i = 0; i < child_index + 1; i++) {
                    int fd = open(name, O_CREATE | O_RDWR);
                    if (fd < 0) {
                        printf("%s: cannot create %s\n", test_name, name);
                        exit(1);
                    }
                    int write_size = sizeof(buffer);
                    int bytes_written = write(fd, buffer, write_size);
                    if (bytes_written != write_size) {
                        printf("%s: write(%d) ret %d\n", test_name,
                               write_size, bytes_written);
                        exit(1);
                    }
                    close(fd);
                }
                unlink(name);
            }

            unlink(name);
            exit(0);
        }
    }

    for (int child_index = 0; child_index < child_count; child_index++) {
        int exit_status = 0;
        wait(&exit_status);
        if (exit_status != 0)
            exit(exit_status);
    }
    exit(0);
}

// 回帰テスト。無効なバッファポインタを使ったwrite()によってファイル用の
// ブロックが割り当てられ、そのファイルを削除しても解放されないか?
// カーネルにこのバグがあると、balloc: out of blocksでpanicする。
// assumed_freeは空きブロック数より大きくする必要があるかもしれない。
// このテストには長い時間がかかる。
void badwrite(char *test_name)
{
    int assumed_free = 600;

    unlink("junk");
    for (int i = 0; i < assumed_free; i++) {
        int fd = open("junk", O_CREATE | O_WRONLY);
        if (fd < 0) {
            printf("open junk failed\n");
            exit(1);
        }
        write(fd, (char *)0xffffffffffL, 1);
        close(fd);
        unlink("junk");
    }

    int fd = open("junk", O_CREATE | O_WRONLY);
    if (fd < 0) {
        printf("open junk failed\n");
        exit(1);
    }
    if (write(fd, "x", 1) != 1) {
        printf("write failed\n");
        exit(1);
    }
    close(fd);
    unlink("junk");

    exit(0);
}

// メモリ不足時に後片付けするexec()コードをテストする。
// 実際には、その状態でpanicしないことをテストする。
void execout(char *test_name)
{
    for (int avail = 0; avail < 15; avail++) {
        int pid = fork();
        if (pid < 0) {
            printf("fork failed\n");
            exit(1);
        } else if (pid == 0) {
            // メモリをすべて割り当てる。
            while (1) {
                char *page = sbrk(PGSIZE);
                if (page == SBRK_ERROR)
                    break;
                *(page + PGSIZE - 1) = 1;
            }

            // exec()が少し進めるよう、数ページを解放する。
            for (int i = 0; i < avail; i++)
                sbrk(-PGSIZE);

            close(1);
            char *arguments[] = {"echo", "x", 0};
            exec("echo", arguments);
            exit(0);
        } else {
            wait((int *)0);
        }
    }

    exit(0);
}

// カーネルはディスク容量不足に耐えられるか?
void diskfull(char *test_name)
{
    int file_index;
    int is_full = 0;

    unlink("diskfulldir");

    for (file_index = 0;
         !is_full && '0' + file_index < 0177;
         file_index++) {
        char name[32];
        name[0] = 'b';
        name[1] = 'i';
        name[2] = 'g';
        name[3] = '0' + file_index;
        name[4] = '\0';
        unlink(name);
        int fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
        if (fd < 0) {
            // おっと、ブロックより先にinodeを使い切った。
            printf("%s: could not create file %s\n", test_name, name);
            is_full = 1;
            break;
        }
        for (int i = 0; i < MAXFILE; i++) {
            char buffer[BSIZE];
            if (write(fd, buffer, BSIZE) != BSIZE) {
                is_full = 1;
                close(fd);
                break;
            }
        }
        close(fd);
    }

    // 空きブロックがない状態で、ディレクトリ内容を拡張できない場合に
    // dirlink()が単に失敗する（panicしない）ことをテストする。
    // これらのファイル作成の1つは失敗するはず。
    int nzz = 128;
    for (int i = 0; i < nzz; i++) {
        char name[32];
        name[0] = 'z';
        name[1] = 'z';
        name[2] = '0' + (i / 32);
        name[3] = '0' + (i % 32);
        name[4] = '\0';
        unlink(name);
        int fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
        if (fd < 0)
            break;
        close(fd);
    }

    // このmkdir()は失敗するはず。
    if (mkdir("diskfulldir") == 0)
        printf("%s: mkdir(diskfulldir) unexpectedly succeeded!\n", test_name);

    unlink("diskfulldir");

    for (int i = 0; i < nzz; i++) {
        char name[32];
        name[0] = 'z';
        name[1] = 'z';
        name[2] = '0' + (i / 32);
        name[3] = '0' + (i % 32);
        name[4] = '\0';
        unlink(name);
    }

    for (int i = 0; '0' + i < 0177; i++) {
        char name[32];
        name[0] = 'b';
        name[1] = 'i';
        name[2] = 'g';
        name[3] = '0' + i;
        name[4] = '\0';
        unlink(name);
    }
}

void outofinodes(char *test_name)
{
    int nzz = 32 * 32;
    for (int i = 0; i < nzz; i++) {
        char name[32];
        name[0] = 'z';
        name[1] = 'z';
        name[2] = '0' + (i / 32);
        name[3] = '0' + (i % 32);
        name[4] = '\0';
        unlink(name);
        int fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
        if (fd < 0) {
            // 最終的には失敗するはず。
            break;
        }
        close(fd);
    }

    for (int i = 0; i < nzz; i++) {
        char name[32];
        name[0] = 'z';
        name[1] = 'z';
        name[2] = '0' + (i / 32);
        name[3] = '0' + (i % 32);
        name[4] = '\0';
        unlink(name);
    }
}

void linkoverflow(char *test_name)
{
    enum { TARGET = 32768 };
    enum { DIRS = 64 };
    struct stat file_status;
    int i;

    unlink("/lof");
    int fd = open("/lof", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf("%s: cannot create /lof\n", test_name);
        exit(1);
    }
    close(fd);

    for (i = 0; i < TARGET; i++) {
        int directory_index = i % DIRS;
        int link_index = i / DIRS;

        char path_name[16];
        path_name[0] = '/';
        path_name[1] = 'd';
        path_name[2] = '_';
        path_name[3] = 'a' + (directory_index / 16);
        path_name[4] = 'a' + (directory_index % 16);
        path_name[5] = '\0';
        if (link_index == 0 && mkdir(path_name) < 0) {
            printf("%s: mkdir(%s) failed\n", test_name, path_name);
            exit(1);
        }

        path_name[5] = '/';
        path_name[6] = 'l';
        path_name[7] = 'a' + (link_index / 256);
        path_name[8] = 'a' + ((link_index / 16) % 16);
        path_name[9] = 'a' + (link_index % 16);
        path_name[10] = '\0';

        if (link("/lof", path_name) < 0) {
            if (stat("/lof", &file_status) < 0) {
                printf("%s: stat(/lof) failed\n", test_name);
                exit(1);
            }
            if (file_status.link_count >= 32767) {
                // オーバーフロー検査に成功した。
                break;
            }
            printf("%s: link failed after %d links (nlink=%d)\n", test_name, i,
                   file_status.link_count);
            exit(1);
        }

        if (i % 100 == 0) {
            printf("%s: i=%d, pn=%s\n", test_name, i, path_name);
        }
    }

    if (stat("/lof", &file_status) < 0) {
        printf("%s: stat(/lof) failed\n", test_name);
        exit(1);
    }

    unlink("/lof");

    if (file_status.link_count < 0) {
        printf("%s: negative link count: %d\n", test_name,
               file_status.link_count);
        exit(1);
    }
}

struct test slow_tests[] = {
    {bigdir, "bigdir"},
    {manywrites, "manywrites"},
    {badwrite, "badwrite"},
    {execout, "execout"},
    {diskfull, "diskfull"},
    {outofinodes, "outofinodes"},
    // {linkoverflow, "linkoverflow"},

    {0, 0},
};

//
// テストを駆動する
//

// 各テストを独自のプロセスで実行する。子のexit()が成功を示せば
// runは1を返す。
int run_test(void test_function(char *), char *test_name)
{
    int pid;
    int exit_status;

    printf("test %s: ", test_name);
    if ((pid = fork()) < 0) {
        printf("runtest: fork error\n");
        exit(1);
    }
    if (pid == 0) {
        test_function(test_name);
        exit(0);
    } else {
        wait(&exit_status);
        if (exit_status != 0)
            printf("FAILED\n");
        else
            printf("OK\n");
        return exit_status == 0;
    }
}

int run_tests(struct test *tests, char *selected_test,
              int continuous_mode)
{
    int test_count = 0;
    for (struct test *test = tests; test->name != 0; test++) {
        if ((selected_test == 0) ||
            strcmp(test->name, selected_test) == 0) {
            test_count++;
            if (!run_test(test->function, test->name)) {
                if (continuous_mode != 2) {
                    printf("SOME TESTS FAILED\n");
                    return -1;
                }
            }
        }
    }
    return test_count;
}

// sbrk()を使って空き物理メモリページ数を数える。
int count_free_pages(void)
{
    int page_count = 0;
    uint64 initial_size = (uint64)sbrk(0);
    while (1) {
        char *page = sbrk(PGSIZE);
        if (page == SBRK_ERROR) {
            break;
        }
        page_count += 1;
    }
    sbrk(-((uint64)sbrk(0) - initial_size));
    return page_count;
}

int drive_tests(int quick_only, int continuous_mode, char *selected_test)
{
    do {
        printf("usertests starting\n");
        int free_pages_before = count_free_pages();
        int free_pages_after = 0;
        int test_count = 0;
        int tests_run;
        tests_run = run_tests(quick_tests, selected_test, continuous_mode);
        if (tests_run < 0) {
            if (continuous_mode != 2) {
                return 1;
            }
        } else {
            test_count += tests_run;
        }
        if (!quick_only) {
            if (selected_test == 0)
                printf("usertests slow tests starting\n");
            tests_run =
                run_tests(slow_tests, selected_test, continuous_mode);
            if (tests_run < 0) {
                if (continuous_mode != 2) {
                    return 1;
                }
            } else {
                test_count += tests_run;
            }
        }
        if ((free_pages_after = count_free_pages()) < free_pages_before) {
            printf("FAILED -- lost some free pages %d (out of %d)\n",
                   free_pages_after, free_pages_before);
            if (continuous_mode != 2) {
                return 1;
            }
        }
        if (selected_test != 0 && test_count == 0) {
            printf("NO TESTS EXECUTED\n");
            return 1;
        }
    } while (continuous_mode);
    return 0;
}

int main(int argc, char *argv[])
{
    int continuous_mode = 0;
    int quick_only = 0;
    char *selected_test = 0;

    if (argc == 2 && strcmp(argv[1], "-q") == 0) {
        quick_only = 1;
    } else if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        continuous_mode = 1;
    } else if (argc == 2 && strcmp(argv[1], "-C") == 0) {
        continuous_mode = 2;
    } else if (argc == 2 && argv[1][0] != '-') {
        selected_test = argv[1];
    } else if (argc > 1) {
        printf("Usage: usertests [-c] [-C] [-q] [testname]\n");
        exit(1);
    }
    if (drive_tests(quick_only, continuous_mode, selected_test)) {
        exit(1);
    }
    printf("ALL TESTS PASSED\n");
    exit(0);
}
