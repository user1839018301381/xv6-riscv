//
// ランダムなシステムコールを並行して永久に実行する。
//

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

// FreeBSD由来。
int generate_random_number(unsigned long *state)
{
    /*
   * 31ビットをオーバーフローさせずに、
   * x = (7^5 * x) mod (2^31 - 1)を計算する:
   *      (2^31 - 1) = 127773 * (7^5) + 2836
   * "Random number generators: good ones are hard to find"より。
   * ParkとMiller、Communications of the ACM、第31巻第10号、
   * 1988年10月、1195ページ。
   */
    long quotient, remainder, value;

    /* [1, 0x7ffffffe]の範囲へ変換する。 */
    value = (*state % 0x7ffffffe) + 1;
    quotient = value / 127773;
    remainder = value % 127773;
    value = 16807 * remainder - 2836 * quotient;
    if (value < 0)
        value += 0x7fffffff;
    /* [0, 0x7ffffffd]の範囲へ変換する。 */
    value--;
    *state = value;
    return value;
}

unsigned long random_state = 1;

int rand(void) { return generate_random_number(&random_state); }

void run_child_workload(int child_index)
{
    int fd = -1;
    static char buffer[999];
    char *initial_program_break = sbrk(0);
    uint64 iteration_count = 0;

    mkdir("grindir");
    if (chdir("grindir") != 0) {
        printf("grind: chdir grindir failed\n");
        exit(1);
    }
    chdir("/");

    while (1) {
        iteration_count++;
        if ((iteration_count % 500) == 0)
            write(1, child_index ? "B" : "A", 1);
        int operation = rand() % 23;
        if (operation == 1) {
            close(open("grindir/../a", O_CREATE | O_RDWR));
        } else if (operation == 2) {
            close(open("grindir/../grindir/../b", O_CREATE | O_RDWR));
        } else if (operation == 3) {
            unlink("grindir/../a");
        } else if (operation == 4) {
            if (chdir("grindir") != 0) {
                printf("grind: chdir grindir failed\n");
                exit(1);
            }
            unlink("../b");
            chdir("/");
        } else if (operation == 5) {
            close(fd);
            fd = open("/grindir/../a", O_CREATE | O_RDWR);
        } else if (operation == 6) {
            close(fd);
            fd = open("/./grindir/./../b", O_CREATE | O_RDWR);
        } else if (operation == 7) {
            write(fd, buffer, sizeof(buffer));
        } else if (operation == 8) {
            read(fd, buffer, sizeof(buffer));
        } else if (operation == 9) {
            mkdir("grindir/../a");
            close(open("a/../a/./a", O_CREATE | O_RDWR));
            unlink("a/a");
        } else if (operation == 10) {
            mkdir("/../b");
            close(open("grindir/../b/b", O_CREATE | O_RDWR));
            unlink("b/b");
        } else if (operation == 11) {
            unlink("b");
            link("../grindir/./../a", "../b");
        } else if (operation == 12) {
            unlink("../grindir/../a");
            link(".././b", "/grindir/../a");
        } else if (operation == 13) {
            int pid = fork();
            if (pid == 0) {
                exit(0);
            } else if (pid < 0) {
                printf("grind: fork failed\n");
                exit(1);
            }
            wait(0);
        } else if (operation == 14) {
            int pid = fork();
            if (pid == 0) {
                fork();
                fork();
                exit(0);
            } else if (pid < 0) {
                printf("grind: fork failed\n");
                exit(1);
            }
            wait(0);
        } else if (operation == 15) {
            sbrk(6011);
        } else if (operation == 16) {
            if (sbrk(0) > initial_program_break)
                sbrk(-(sbrk(0) - initial_program_break));
        } else if (operation == 17) {
            int pid = fork();
            if (pid == 0) {
                close(open("a", O_CREATE | O_RDWR));
                exit(0);
            } else if (pid < 0) {
                printf("grind: fork failed\n");
                exit(1);
            }
            if (chdir("../grindir/..") != 0) {
                printf("grind: chdir failed\n");
                exit(1);
            }
            kill(pid);
            wait(0);
        } else if (operation == 18) {
            int pid = fork();
            if (pid == 0) {
                kill(getpid());
                exit(0);
            } else if (pid < 0) {
                printf("grind: fork failed\n");
                exit(1);
            }
            wait(0);
        } else if (operation == 19) {
            int pipe_fds[2];
            if (pipe(pipe_fds) < 0) {
                printf("grind: pipe failed\n");
                exit(1);
            }
            int pid = fork();
            if (pid == 0) {
                fork();
                fork();
                if (write(pipe_fds[1], "x", 1) != 1)
                    printf("grind: pipe write failed\n");
                char byte;
                if (read(pipe_fds[0], &byte, 1) != 1)
                    printf("grind: pipe read failed\n");
                exit(0);
            } else if (pid < 0) {
                printf("grind: fork failed\n");
                exit(1);
            }
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            wait(0);
        } else if (operation == 20) {
            int pid = fork();
            if (pid == 0) {
                unlink("a");
                mkdir("a");
                chdir("a");
                unlink("../a");
                fd = open("x", O_CREATE | O_RDWR);
                unlink("x");
                exit(0);
            } else if (pid < 0) {
                printf("grind: fork failed\n");
                exit(1);
            }
            wait(0);
        } else if (operation == 21) {
            unlink("c");
            // 常に成功するはず。空きinode、ファイルディスクリプタ、
            // ブロックがあることを確認する。
            int test_file_fd = open("c", O_CREATE | O_RDWR);
            if (test_file_fd < 0) {
                printf("grind: create c failed\n");
                exit(1);
            }
            if (write(test_file_fd, "x", 1) != 1) {
                printf("grind: write c failed\n");
                exit(1);
            }
            struct stat file_status;
            if (fstat(test_file_fd, &file_status) != 0) {
                printf("grind: fstat failed\n");
                exit(1);
            }
            if (file_status.size != 1) {
                printf("grind: fstat reports wrong size %d\n",
                       (int)file_status.size);
                exit(1);
            }
            if (file_status.inode_number > 200) {
                printf("grind: fstat reports crazy i-number %d\n",
                       file_status.inode_number);
                exit(1);
            }
            close(test_file_fd);
            unlink("c");
        } else if (operation == 22) {
            // echo hi | cat
            int input_pipe[2], output_pipe[2];
            if (pipe(input_pipe) < 0) {
                fprintf(2, "grind: pipe failed\n");
                exit(1);
            }
            if (pipe(output_pipe) < 0) {
                fprintf(2, "grind: pipe failed\n");
                exit(1);
            }
            int pid1 = fork();
            if (pid1 == 0) {
                close(output_pipe[0]);
                close(output_pipe[1]);
                close(input_pipe[0]);
                close(1);
                if (dup(input_pipe[1]) != 1) {
                    fprintf(2, "grind: dup failed\n");
                    exit(1);
                }
                close(input_pipe[1]);
                char *arguments[3] = {"echo", "hi", 0};
                exec("grindir/../echo", arguments);
                fprintf(2, "grind: echo: not found\n");
                exit(2);
            } else if (pid1 < 0) {
                fprintf(2, "grind: fork failed\n");
                exit(3);
            }
            int pid2 = fork();
            if (pid2 == 0) {
                close(input_pipe[1]);
                close(output_pipe[0]);
                close(0);
                if (dup(input_pipe[0]) != 0) {
                    fprintf(2, "grind: dup failed\n");
                    exit(4);
                }
                close(input_pipe[0]);
                close(1);
                if (dup(output_pipe[1]) != 1) {
                    fprintf(2, "grind: dup failed\n");
                    exit(5);
                }
                close(output_pipe[1]);
                char *arguments[2] = {"cat", 0};
                exec("/cat", arguments);
                fprintf(2, "grind: cat: not found\n");
                exit(6);
            } else if (pid2 < 0) {
                fprintf(2, "grind: fork failed\n");
                exit(7);
            }
            close(input_pipe[0]);
            close(input_pipe[1]);
            close(output_pipe[1]);
            char output[4] = {0, 0, 0, 0};
            read(output_pipe[0], output + 0, 1);
            read(output_pipe[0], output + 1, 1);
            read(output_pipe[0], output + 2, 1);
            close(output_pipe[0]);
            int first_exit_status, second_exit_status;
            wait(&first_exit_status);
            wait(&second_exit_status);
            if (first_exit_status != 0 || second_exit_status != 0 ||
                strcmp(output, "hi\n") != 0) {
                printf("grind: exec pipeline failed %d %d \"%s\"\n",
                       first_exit_status, second_exit_status, output);
                exit(1);
            }
        }
    }
}

void run_iteration(void)
{
    unlink("a");
    unlink("b");

    int pid1 = fork();
    if (pid1 < 0) {
        printf("grind: fork failed\n");
        exit(1);
    }
    if (pid1 == 0) {
        random_state ^= 31;
        run_child_workload(0);
        exit(0);
    }

    int pid2 = fork();
    if (pid2 < 0) {
        printf("grind: fork failed\n");
        exit(1);
    }
    if (pid2 == 0) {
        random_state ^= 7177;
        run_child_workload(1);
        exit(0);
    }

    int first_exit_status = -1;
    wait(&first_exit_status);
    if (first_exit_status != 0) {
        kill(pid1);
        kill(pid2);
    }
    int second_exit_status = -1;
    wait(&second_exit_status);

    exit(0);
}

int main()
{
    while (1) {
        int pid = fork();
        if (pid == 0) {
            run_iteration();
            exit(0);
        }
        if (pid > 0) {
            wait(0);
        }
        pause(20);
        random_state += 1;
    }
}
