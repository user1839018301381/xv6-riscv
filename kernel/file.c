//
// ファイルディスクリプタを使うシステムコールのための補助関数群。
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
    struct spinlock lock;
    struct file files[NFILE];
} file_table;

void fileinit(void) { initlock(&file_table.lock, "file_table"); }

// ファイル構造体を1つ割り当てる。
struct file *filealloc(void)
{
    struct file *file;

    acquire(&file_table.lock);
    for (file = file_table.files; file < file_table.files + NFILE; file++) {
        if (file->reference_count == 0) {
            file->reference_count = 1;
            release(&file_table.lock);
            return file;
        }
    }
    release(&file_table.lock);
    return 0;
}

// ファイルの参照カウントを増やす。
struct file *filedup(struct file *file)
{
    acquire(&file_table.lock);
    if (file->reference_count < 1)
        panic("filedup");
    file->reference_count++;
    release(&file_table.lock);
    return file;
}

// ファイルを閉じる(参照カウントを減らし、0になったら閉じる)。
void fileclose(struct file *file)
{
    struct file closed_file;

    acquire(&file_table.lock);
    if (file->reference_count < 1)
        panic("fileclose");
    if (--file->reference_count > 0) {
        release(&file_table.lock);
        return;
    }
    closed_file = *file;
    file->reference_count = 0;
    file->type = FD_NONE;
    release(&file_table.lock);

    if (closed_file.type == FD_PIPE) {
        pipeclose(closed_file.pipe, closed_file.is_writable);
    } else if (closed_file.type == FD_INODE || closed_file.type == FD_DEVICE) {
        begin_op();
        iput(closed_file.inode);
        end_op();
    }
}

// ファイルのメタデータを読み出す。
// status_addressはstruct statを指すユーザ仮想アドレス。
int filestat(struct file *file, uint64 status_address)
{
    struct proc *process = myproc();
    struct stat file_status;

    if (file->type == FD_INODE || file->type == FD_DEVICE) {
        ilock(file->inode);
        stati(file->inode, &file_status);
        iunlock(file->inode);
        if (copyout(process->pagetable, process->memory_size, status_address,
                    (char *)&file_status, sizeof(file_status)) < 0)
            return -1;
        return 0;
    }
    return -1;
}

// ファイルから読み出す。
// destination_addressは読み出し先のユーザ仮想アドレス。
int fileread(struct file *file, uint64 destination_address, int byte_count)
{
    int bytes_read = 0;

    if (file->is_readable == 0 || byte_count < 0)
        return -1;

    if (file->type == FD_PIPE) {
        bytes_read = piperead(file->pipe, destination_address, byte_count);
    } else if (file->type == FD_DEVICE) {
        if (file->major < 0 || file->major >= NDEV || !devsw[file->major].read)
            return -1;
        bytes_read = devsw[file->major].read(1, destination_address, byte_count);
    } else if (file->type == FD_INODE) {
        ilock(file->inode);
        if ((bytes_read = readi(file->inode, 1, destination_address, file->offset, byte_count)) > 0)
            file->offset += bytes_read;
        iunlock(file->inode);
    } else {
        panic("fileread");
    }

    return bytes_read;
}

// ファイルへ書き込む。
// source_addressは書き込み元のユーザ仮想アドレス。
int filewrite(struct file *file, uint64 source_address, int byte_count)
{
    int bytes_written = 0;

    if (file->is_writable == 0 || byte_count < 0)
        return -1;

    if (file->type == FD_PIPE) {
        bytes_written = pipewrite(file->pipe, source_address, byte_count);
    } else if (file->type == FD_DEVICE) {
        if (file->major < 0 || file->major >= NDEV || !devsw[file->major].write)
            return -1;
        bytes_written = devsw[file->major].write(1, source_address, byte_count);
    } else if (file->type == FD_INODE) {
        // ログの最大トランザクションサイズを超えないよう数ブロックずつ書く。
        // iノード・間接ブロック・確保ブロックに加え、
        // 非整列書き込み用の余裕2ブロック分を考慮する。
        int max_bytes_per_transaction = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
        int total_bytes_written = 0;
        while (total_bytes_written < byte_count) {
            int transaction_byte_count = byte_count - total_bytes_written;
            if (transaction_byte_count > max_bytes_per_transaction)
                transaction_byte_count = max_bytes_per_transaction;

            begin_op();
            ilock(file->inode);
            int transaction_bytes_written =
                writei(file->inode, 1, source_address + total_bytes_written,
                       file->offset, transaction_byte_count);
            if (transaction_bytes_written > 0)
                file->offset += transaction_bytes_written;
            iunlock(file->inode);
            end_op();

            if (transaction_bytes_written != transaction_byte_count) {
                // writeiによるエラー。
                break;
            }
            total_bytes_written += transaction_bytes_written;
        }
        bytes_written = (total_bytes_written == byte_count ? byte_count : -1);
    } else {
        panic("filewrite");
    }

    return bytes_written;
}
