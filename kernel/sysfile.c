//
// ファイルシステムのシステムコール。
// ユーザコードを信頼しないため主に引数を検査し、
// file.cとfs.cを呼び出す。
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// argument_index番目のワードサイズのシステムコール引数を
// ファイルディスクリプタとして読み出し、
// ディスクリプタと対応するstruct fileの両方を返す。
static int argfd(int argument_index, int *fd_out, struct file **file_out)
{
    int fd;
    struct file *file;

    argint(argument_index, &fd);
    if (fd < 0 || fd >= NOFILE || (file = myproc()->open_files[fd]) == 0)
        return -1;
    if (fd_out)
        *fd_out = fd;
    if (file_out)
        *file_out = file;
    return 0;
}

// 指定されたファイル用のファイルディスクリプタを割り当てる。
// 成功時は呼び出し元からファイル参照を引き継ぐ。
static int fdalloc(struct file *file)
{
    int fd;
    struct proc *process = myproc();

    for (fd = 0; fd < NOFILE; fd++) {
        if (process->open_files[fd] == 0) {
            process->open_files[fd] = file;
            return fd;
        }
    }
    return -1;
}

uint64 sys_dup(void)
{
    struct file *file;
    int fd;

    if (argfd(0, 0, &file) < 0)
        return -1;
    if ((fd = fdalloc(file)) < 0)
        return -1;
    filedup(file);
    return fd;
}

uint64 sys_read(void)
{
    struct file *file;
    int byte_count;
    uint64 destination_address;

    argaddr(1, &destination_address);
    argint(2, &byte_count);
    if (argfd(0, 0, &file) < 0)
        return -1;
    return fileread(file, destination_address, byte_count);
}

uint64 sys_write(void)
{
    struct file *file;
    int byte_count;
    uint64 source_address;

    argaddr(1, &source_address);
    argint(2, &byte_count);
    if (argfd(0, 0, &file) < 0)
        return -1;

    return filewrite(file, source_address, byte_count);
}

uint64 sys_close(void)
{
    int fd;
    struct file *file;

    if (argfd(0, &fd, &file) < 0)
        return -1;
    myproc()->open_files[fd] = 0;
    fileclose(file);
    return 0;
}

uint64 sys_fstat(void)
{
    struct file *file;
    uint64 status_address;

    argaddr(1, &status_address);
    if (argfd(0, 0, &file) < 0)
        return -1;
    return filestat(file, status_address);
}

// new_pathをold_pathと同じinodeへのリンクとして作成する。
uint64 sys_link(void)
{
    char entry_name[DIRSIZ], new_path[MAXPATH], old_path[MAXPATH];
    struct inode *parent_inode, *target_inode;

    if (argstr(0, old_path, MAXPATH) < 0 ||
        argstr(1, new_path, MAXPATH) < 0)
        return -1;

    begin_op();
    if ((target_inode = namei(old_path)) == 0) {
        end_op();
        return -1;
    }

    ilock(target_inode);
    if (target_inode->type == T_DIR) {
        iunlockput(target_inode);
        end_op();
        return -1;
    }

    if (target_inode->link_count >= NLINK_MAX) {
        iunlockput(target_inode);
        end_op();
        return -1;
    }

    target_inode->link_count++;
    iupdate(target_inode);
    iunlock(target_inode);

    if ((parent_inode = nameiparent(new_path, entry_name)) == 0)
        goto link_failed;
    ilock(parent_inode);
    // 解決中にparent_inodeのリンクが解除された可能性がある。
    // 孤児ディレクトリへリンクすると、target_inode->link_countを減らさず
    // itruncがレコードを破棄するためtarget_inodeがリークする。
    // create()にも同じガードがある。
    if (parent_inode->link_count == 0) {
        iunlockput(parent_inode);
        goto link_failed;
    }
    if (parent_inode->device != target_inode->device ||
        dirlink(parent_inode, entry_name, target_inode->inode_number) < 0) {
        iunlockput(parent_inode);
        goto link_failed;
    }
    iunlockput(parent_inode);
    iput(target_inode);

    end_op();

    return 0;

link_failed:
    ilock(target_inode);
    target_inode->link_count--;
    iupdate(target_inode);
    iunlockput(target_inode);
    end_op();
    return -1;
}

// ディレクトリは"."と".."以外が空か?
static int is_directory_empty(struct inode *directory_inode)
{
    int entry_offset;
    struct dirent directory_entry;

    for (entry_offset = 2 * sizeof(directory_entry);
         entry_offset < directory_inode->size;
         entry_offset += sizeof(directory_entry)) {
        if (readi(directory_inode, 0, (uint64)&directory_entry,
                  entry_offset, sizeof(directory_entry)) !=
            sizeof(directory_entry))
            panic("is_directory_empty: readi");
        if (directory_entry.inode_number != 0)
            return 0;
    }
    return 1;
}

uint64 sys_unlink(void)
{
    struct inode *target_inode, *parent_inode;
    struct dirent empty_entry;
    char entry_name[DIRSIZ], path[MAXPATH];
    uint entry_offset;

    if (argstr(0, path, MAXPATH) < 0)
        return -1;

    begin_op();
    if ((parent_inode = nameiparent(path, entry_name)) == 0) {
        end_op();
        return -1;
    }

    ilock(parent_inode);

    // "."や".."のリンクは解除できない。
    if (namecmp(entry_name, ".") == 0 || namecmp(entry_name, "..") == 0)
        goto unlink_failed;

    if ((target_inode = dirlookup(parent_inode, entry_name, &entry_offset)) == 0)
        goto unlink_failed;
    ilock(target_inode);

    if (target_inode->link_count < 1)
        panic("unlink: nlink < 1");
    if (target_inode->type == T_DIR && !is_directory_empty(target_inode)) {
        iunlockput(target_inode);
        goto unlink_failed;
    }

    memset(&empty_entry, 0, sizeof(empty_entry));
    if (writei(parent_inode, 0, (uint64)&empty_entry, entry_offset,
               sizeof(empty_entry)) != sizeof(empty_entry))
        panic("unlink: writei");
    if (target_inode->type == T_DIR) {
        parent_inode->link_count--;
        iupdate(parent_inode);
    }
    iunlockput(parent_inode);

    target_inode->link_count--;
    iupdate(target_inode);
    iunlockput(target_inode);

    end_op();

    return 0;

unlink_failed:
    iunlockput(parent_inode);
    end_op();
    return -1;
}

static struct inode *create_inode(char *path, short inode_type,
                                  short major, short minor)
{
    struct inode *inode, *parent_inode;
    char entry_name[DIRSIZ];

    if ((parent_inode = nameiparent(path, entry_name)) == 0)
        return 0;

    ilock(parent_inode);

    if (parent_inode->link_count == 0) {
        iunlockput(parent_inode);
        return 0;
    }

    // 新しいディレクトリの".."によってparent_inode->link_countが最大値を超える
    if (inode_type == T_DIR && parent_inode->link_count >= NLINK_MAX) {
        iunlockput(parent_inode);
        return 0;
    }

    if ((inode = dirlookup(parent_inode, entry_name, 0)) != 0) {
        iunlockput(parent_inode);
        ilock(inode);
        if (inode_type == T_FILE &&
            (inode->type == T_FILE || inode->type == T_DEVICE))
            return inode;
        iunlockput(inode);
        return 0;
    }

    if ((inode = ialloc(parent_inode->device, inode_type)) == 0) {
        iunlockput(parent_inode);
        return 0;
    }

    ilock(inode);
    inode->major = major;
    inode->minor = minor;
    inode->link_count = 1;
    iupdate(inode);

    if (inode_type == T_DIR) { // .と..のエントリを作成する。
        // 循環参照カウントを避けるため、"."ではinode->link_count++しない。
        if (dirlink(inode, ".", inode->inode_number) < 0 ||
            dirlink(inode, "..", parent_inode->inode_number) < 0)
            goto creation_failed;
    }

    if (dirlink(parent_inode, entry_name, inode->inode_number) < 0)
        goto creation_failed;

    if (inode_type == T_DIR) {
        // 成功が保証されたので:
        parent_inode->link_count++; // ".."用
        iupdate(parent_inode);
    }

    iunlockput(parent_inode);

    return inode;

creation_failed:
    // 問題が起きたのでinodeの割り当てを解除する。
    inode->link_count = 0;
    iupdate(inode);
    iunlockput(inode);
    iunlockput(parent_inode);
    return 0;
}

uint64 sys_open(void)
{
    char path[MAXPATH];
    int fd, open_mode;
    struct file *file;
    struct inode *inode;

    argint(1, &open_mode);
    if (argstr(0, path, MAXPATH) < 0)
        return -1;

    begin_op();

    if (open_mode & O_CREATE) {
        inode = create_inode(path, T_FILE, 0, 0);
        if (inode == 0) {
            end_op();
            return -1;
        }
    } else {
        if ((inode = namei(path)) == 0) {
            end_op();
            return -1;
        }
        ilock(inode);
        if (inode->type == T_DIR && open_mode != O_RDONLY) {
            iunlockput(inode);
            end_op();
            return -1;
        }
    }

    if (inode->type == T_DEVICE &&
        (inode->major < 0 || inode->major >= NDEV)) {
        iunlockput(inode);
        end_op();
        return -1;
    }

    if ((file = filealloc()) == 0 || (fd = fdalloc(file)) < 0) {
        if (file)
            fileclose(file);
        iunlockput(inode);
        end_op();
        return -1;
    }

    if (inode->type == T_DEVICE) {
        file->type = FD_DEVICE;
        file->major = inode->major;
    } else {
        file->type = FD_INODE;
        file->offset = 0;
    }
    file->inode = inode;
    file->is_readable = !(open_mode & O_WRONLY);
    file->is_writable = (open_mode & O_WRONLY) || (open_mode & O_RDWR);

    if ((open_mode & O_TRUNC) && inode->type == T_FILE) {
        itrunc(inode);
    }

    iunlock(inode);
    end_op();

    return fd;
}

uint64 sys_mkdir(void)
{
    char path[MAXPATH];
    struct inode *directory_inode;

    begin_op();
    if (argstr(0, path, MAXPATH) < 0 ||
        (directory_inode = create_inode(path, T_DIR, 0, 0)) == 0) {
        end_op();
        return -1;
    }
    iunlockput(directory_inode);
    end_op();
    return 0;
}

uint64 sys_mknod(void)
{
    struct inode *device_inode;
    char path[MAXPATH];
    int major, minor;

    begin_op();
    argint(1, &major);
    argint(2, &minor);
    if ((argstr(0, path, MAXPATH)) < 0 ||
        (device_inode = create_inode(path, T_DEVICE, major, minor)) == 0) {
        end_op();
        return -1;
    }
    iunlockput(device_inode);
    end_op();
    return 0;
}

uint64 sys_chdir(void)
{
    char path[MAXPATH];
    struct inode *directory_inode;
    struct proc *process = myproc();

    begin_op();
    if (argstr(0, path, MAXPATH) < 0 ||
        (directory_inode = namei(path)) == 0) {
        end_op();
        return -1;
    }
    ilock(directory_inode);
    if (directory_inode->type != T_DIR) {
        iunlockput(directory_inode);
        end_op();
        return -1;
    }
    iunlock(directory_inode);
    iput(process->current_directory);
    end_op();
    process->current_directory = directory_inode;
    return 0;
}

uint64 sys_exec(void)
{
    char path[MAXPATH], *argv[MAXARG];
    int i;
    uint64 user_argv_address, user_argument_address;

    argaddr(1, &user_argv_address);
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    for (i = 0;; i++) {
        if (i >= NELEM(argv)) {
            goto exec_failed;
        }
        if (fetchaddr(user_argv_address + sizeof(uint64) * i,
                      &user_argument_address) < 0) {
            goto exec_failed;
        }
        if (user_argument_address == 0) {
            argv[i] = 0;
            break;
        }
        argv[i] = kalloc();
        if (argv[i] == 0)
            goto exec_failed;
        if (fetchstr(user_argument_address, argv[i], PGSIZE) < 0)
            goto exec_failed;
    }

    int exec_status = kexec(path, argv);

    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);

    return exec_status;

exec_failed:
    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
        kfree(argv[i]);
    return -1;
}

uint64 sys_pipe(void)
{
    uint64 fd_array_address;
    struct file *read_file, *write_file;
    int read_fd, write_fd;
    struct proc *process = myproc();

    argaddr(0, &fd_array_address);
    if (pipealloc(&read_file, &write_file) < 0)
        return -1;
    read_fd = -1;
    if ((read_fd = fdalloc(read_file)) < 0 ||
        (write_fd = fdalloc(write_file)) < 0) {
        if (read_fd >= 0)
            process->open_files[read_fd] = 0;
        fileclose(read_file);
        fileclose(write_file);
        return -1;
    }
    if (copyout(process->pagetable, process->memory_size, fd_array_address,
                (char *)&read_fd, sizeof(read_fd)) < 0 ||
        copyout(process->pagetable, process->memory_size,
                fd_array_address + sizeof(read_fd), (char *)&write_fd,
                sizeof(write_fd)) < 0) {
        process->open_files[read_fd] = 0;
        process->open_files[write_fd] = 0;
        fileclose(read_file);
        fileclose(write_file);
        return -1;
    }
    return 0;
}
