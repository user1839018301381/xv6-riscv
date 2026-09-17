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
    struct buf buf[NBUF];

    // prev/nextで全バッファをつなぐ連結リスト。
    // バッファの使用が新しい順に整列される。
    // head.nextが最新、head.prevが最古。
    struct buf head;
} bcache;

void binit(void)
{
    struct buf *b;

    initlock(&bcache.lock, "bcache");

    // バッファの連結リストを作成する
    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        initsleeplock(&b->lock, "buffer");
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

// デバイスdev上のブロックをバッファキャッシュから探す。
// 見つからなければバッファを確保する。
// いずれの場合もロック済みバッファを返す。
static struct buf *bget(uint dev, uint blockno)
{
    struct buf *b;

    acquire(&bcache.lock);

    // このブロックは既にキャッシュされているか?
    for (b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // キャッシュにない。
    // 未使用のうち最も古く使われた(LRU)バッファを再利用する。
    for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (b->refcnt == 0) {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    panic("bget: no buffers");
}

// 指定ブロックの内容を持つロック済みバッファを返す。
struct buf *bread(uint dev, uint blockno)
{
    struct buf *b;

    b = bget(dev, blockno);
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// バッファbの内容をディスクに書き込む。ロック済みであること。
// bwriteを呼ぶのはログ層のみ。
void bwrite(struct buf *b)
{
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// ロック済みバッファを解放する。
// 最近使ったバッファの先頭(MRU)へ移動する。
void brelse(struct buf *b)
{
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock);

    acquire(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {
        // このバッファを待っている者はいない。
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }

    release(&bcache.lock);
}

void bpin(struct buf *b)
{
    acquire(&bcache.lock);
    b->refcnt++;
    release(&bcache.lock);
}

void bunpin(struct buf *b)
{
    acquire(&bcache.lock);
    b->refcnt--;
    release(&bcache.lock);
}
