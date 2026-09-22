// バッファキャッシュ。
//
// バッファキャッシュはディスクブロックの内容の複製を保持する
// buf構造体の連結リストである。ディスクブロックをメモリに
// キャッシュすることでディスク読み込み回数を減らし、複数プロセスが
// 使用するディスクブロックの同期点も提供する。
//
// 使い方:
// * 特定のディスクブロック用バッファを得るにはbreadを呼ぶ。
// * バッファ内容を変更したらbwriteでディスクに書き戻す。
// * 使い終わったらbrelseを呼ぶ。
// * brelse後はバッファを使ってはならない。
// * 一度に使えるのは1プロセスのみなので、
//     必要以上に保持しないこと。

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
    struct spinlock lock;
    struct buf buffers[NBUF];

    // prev/nextで全バッファをつなぐ連結リスト。
    // バッファの使用が新しい順に整列される。
    // head.nextが最新、head.previousが最古。
    struct buf head;
} buffer_cache;

void binit(void)
{
    struct buf *buffer;

    initlock(&buffer_cache.lock, "buffer_cache");

    // バッファの連結リストを作成する
    buffer_cache.head.previous = &buffer_cache.head;
    buffer_cache.head.next = &buffer_cache.head;
    for (buffer = buffer_cache.buffers;
         buffer < buffer_cache.buffers + NBUF; buffer++) {
        buffer->next = buffer_cache.head.next;
        buffer->previous = &buffer_cache.head;
        initsleeplock(&buffer->lock, "buffer");
        buffer_cache.head.next->previous = buffer;
        buffer_cache.head.next = buffer;
    }
}

// デバイスdev上のブロックをバッファキャッシュから探す。
// 見つからなければバッファを確保する。
// いずれの場合もロック済みバッファを返す。
static struct buf *acquire_buffer(uint device, uint block_number)
{
    struct buf *buffer;

    acquire(&buffer_cache.lock);

    // このブロックは既にキャッシュされているか?
    for (buffer = buffer_cache.head.next;
         buffer != &buffer_cache.head; buffer = buffer->next) {
        if (buffer->device == device && buffer->block_number == block_number) {
            buffer->reference_count++;
            release(&buffer_cache.lock);
            acquiresleep(&buffer->lock);
            return buffer;
        }
    }

    // キャッシュにない。
    // 未使用のうち最も古く使われた(LRU)バッファを再利用する。
    for (buffer = buffer_cache.head.previous;
         buffer != &buffer_cache.head; buffer = buffer->previous) {
        if (buffer->reference_count == 0) {
            buffer->device = device;
            buffer->block_number = block_number;
            buffer->is_valid = 0;
            buffer->reference_count = 1;
            release(&buffer_cache.lock);
            acquiresleep(&buffer->lock);
            return buffer;
        }
    }
    panic("acquire_buffer: no buffers");
}

// 指定ブロックの内容を持つロック済みバッファを返す。
struct buf *bread(uint device, uint block_number)
{
    struct buf *buffer;

    buffer = acquire_buffer(device, block_number);
    if (!buffer->is_valid) {
        virtio_disk_rw(buffer, 0);
        buffer->is_valid = 1;
    }
    return buffer;
}

// バッファの内容をディスクに書き込む。ロック済みであること。
// bwriteを呼ぶのはログ層のみ。
void bwrite(struct buf *buffer)
{
    if (!holdingsleep(&buffer->lock))
        panic("bwrite");
    virtio_disk_rw(buffer, 1);
}

// ロック済みバッファを解放する。
// 最近使ったバッファの先頭(MRU)へ移動する。
void brelse(struct buf *buffer)
{
    if (!holdingsleep(&buffer->lock))
        panic("brelse");

    releasesleep(&buffer->lock);

    acquire(&buffer_cache.lock);
    buffer->reference_count--;
    if (buffer->reference_count == 0) {
        // このバッファを待っている者はいない。
        buffer->next->previous = buffer->previous;
        buffer->previous->next = buffer->next;
        buffer->next = buffer_cache.head.next;
        buffer->previous = &buffer_cache.head;
        buffer_cache.head.next->previous = buffer;
        buffer_cache.head.next = buffer;
    }

    release(&buffer_cache.lock);
}

void bpin(struct buf *buffer)
{
    acquire(&buffer_cache.lock);
    buffer->reference_count++;
    release(&buffer_cache.lock);
}

void bunpin(struct buf *buffer)
{
    acquire(&buffer_cache.lock);
    buffer->reference_count--;
    release(&buffer_cache.lock);
}
