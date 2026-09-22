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
    struct file file[NFILE];
} ftable;

void fileinit(void) { initlock(&ftable.lock, "ftable"); }

// ファイル構造体を1つ割り当てる。
struct file *filealloc(void)
{
    struct file *f;

    acquire(&ftable.lock);
    for (f = ftable.file; f < ftable.file + NFILE; f++) {
        if (f->ref == 0) {
            f->ref = 1;
            release(&ftable.lock);
            return f;
        }
    }
    release(&ftable.lock);
    return 0;
}

// ファイルfの参照カウントを増やす。
struct file *filedup(struct file *f)
{
    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("filedup");
    f->ref++;
    release(&ftable.lock);
    return f;
}

// ファイルfを閉じる(参照カウントを減らし、0になったら閉じる)。
void fileclose(struct file *f)
{
    struct file ff;

    acquire(&ftable.lock);
    if (f->ref < 1)
        panic("fileclose");
    if (--f->ref > 0) {
        release(&ftable.lock);
        return;
    }
    ff = *f;
    f->ref = 0;
    f->type = FD_NONE;
    release(&ftable.lock);

    if (ff.type == FD_PIPE) {
        pipeclose(ff.pipe, ff.writable);
    } else if (ff.type == FD_INODE || ff.type == FD_DEVICE) {
        begin_op();
        iput(ff.ip);
        end_op();
    }
}

// ファイルfのメタデータを取得する。
// addrはstruct statを指すユーザ仮想アドレス。
int filestat(struct file *f, uint64 addr)
{
    struct proc *p = myproc();
    struct stat st;

    if (f->type == FD_INODE || f->type == FD_DEVICE) {
        ilock(f->ip);
        stati(f->ip, &st);
        iunlock(f->ip);
        if (copyout(p->pagetable, p->sz, addr, (char *)&st, sizeof(st)) < 0)
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

    if (file->readable == 0 || byte_count < 0)
        return -1;

    if (file->type == FD_PIPE) {
        bytes_read = piperead(file->pipe, destination_address, byte_count);
    } else if (file->type == FD_DEVICE) {
        if (file->major < 0 || file->major >= NDEV || !devsw[file->major].read)
            return -1;
        bytes_read = devsw[file->major].read(1, destination_address, byte_count);
    } else if (file->type == FD_INODE) {
        ilock(file->ip);
        if ((bytes_read = readi(file->ip, 1, destination_address, file->off, byte_count)) > 0)
            file->off += bytes_read;
        iunlock(file->ip);
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

    if (file->writable == 0 || byte_count < 0)
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
            ilock(file->ip);
            int transaction_bytes_written =
                writei(file->ip, 1, source_address + total_bytes_written,
                       file->off, transaction_byte_count);
            if (transaction_bytes_written > 0)
                file->off += transaction_bytes_written;
            iunlock(file->ip);
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
