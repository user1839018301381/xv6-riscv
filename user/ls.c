#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char *format_name(char *path)
{
    static char formatted_name[DIRSIZ + 1];
    char *name_start;

    // 最後のスラッシュの直後の最初の文字を探す。
    for (name_start = path + strlen(path);
         name_start >= path && *name_start != '/'; name_start--)
        ;
    name_start++;

    // 空白埋めした名前を返す。
    if (strlen(name_start) >= DIRSIZ)
        return name_start;
    memmove(formatted_name, name_start, strlen(name_start));
    memset(formatted_name + strlen(name_start), ' ',
           DIRSIZ - strlen(name_start));
    formatted_name[sizeof(formatted_name) - 1] = '\0';
    return formatted_name;
}

void ls(char *path)
{
    char path_buffer[512], *path_end;
    int fd;
    struct dirent directory_entry;
    struct stat file_status;

    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &file_status) < 0) {
        fprintf(2, "ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (file_status.type) {
    case T_DEVICE:
    case T_FILE:
        printf("%s %d %d %d\n", format_name(path), file_status.type,
               file_status.inode_number, (int)file_status.size);
        break;

    case T_DIR:
        if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(path_buffer)) {
            printf("ls: path too long\n");
            break;
        }
        strcpy(path_buffer, path);
        path_end = path_buffer + strlen(path_buffer);
        *path_end++ = '/';
        while (read(fd, &directory_entry, sizeof(directory_entry)) ==
               sizeof(directory_entry)) {
            if (directory_entry.inode_number == 0)
                continue;
            memmove(path_end, directory_entry.name, DIRSIZ);
            path_end[DIRSIZ] = 0;
            if (stat(path_buffer, &file_status) < 0) {
                printf("ls: cannot stat %s\n", path_buffer);
                continue;
            }
            printf("%s %d %d %d\n", format_name(path_buffer), file_status.type,
                   file_status.inode_number, (int)file_status.size);
        }
        break;
    }
    close(fd);
}

int main(int argc, char *argv[])
{
    int i;

    if (argc < 2) {
        ls(".");
        exit(0);
    }
    for (i = 1; i < argc; i++)
        ls(argv[i]);
    exit(0);
}
