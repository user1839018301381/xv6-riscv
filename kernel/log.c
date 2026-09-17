#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// 複数のFSシステムコールを並行実行できる単純なログ機構。
//
// 1つのログトランザクションには複数のFSシステムコールによる更新が含まれる。
// ログ機構はFSシステムコールが実行中でないときだけコミットする。
// そのため、コミットによって未コミットのシステムコールの更新が
// ディスクに書かれる可能性を考える必要はない。
//
// システムコールは開始時と終了時にbegin_op()/end_op()を呼ぶ。
// 通常、begin_op()は実行中のFSシステムコール数を増やして戻る。
// ただしログが満杯に近いと判断した場合は、
// 最後に残ったend_op()がコミットするまでスリープする。
//
// ログはディスクブロックを含む物理的な再実行ログである。
// ディスク上のログ形式:
//   ブロックA、B、C、...の番号を含むヘッダブロック
//   ブロックA
//   ブロックB
//   ブロックC
//   ...
// ログへの追加は同期的に行う。

// ヘッダブロックの内容。
// ディスク上のヘッダブロックと、コミット前にログ対象ブロック番号を
// メモリ上で追跡するための両方に使う。
struct logheader {
    int n;
    int block[LOGBLOCKS];
};

struct log {
    struct spinlock lock;
    int start;
    int outstanding; // 実行中のFSシステムコール数。
    int committing;  // commit()中なので待機する。
    int dev;
    int ncommit;
    struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

void initlog(int dev, struct superblock *sb)
{
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    initlock(&log.lock, "log");
    log.start = sb->logstart;
    log.dev = dev;
    recover_from_log();
}

// コミット済みのブロックをログから本来の場所へ複写する。
static void install_trans(int recovering)
{
    int tail;

    for (tail = 0; tail < log.lh.n; tail++) {
        if (recovering) {
            printk("recovering tail %d dst %d\n", tail, log.lh.block[tail]);
        }
        struct buf *lbuf =
            bread(log.dev, log.start + tail + 1); // ログブロックを読む
        struct buf *dbuf =
            bread(log.dev, log.lh.block[tail]); // 書き込み先を読む
        memmove(dbuf->data, lbuf->data, BSIZE); // ブロックを書き込み先へ複写
        bwrite(dbuf);                           // 書き込み先をディスクへ書く
        if (recovering == 0)
            bunpin(dbuf);
        brelse(lbuf);
        brelse(dbuf);
    }
}

// ディスク上のログヘッダをメモリ上のログヘッダに読み込む。
static void read_head(void)
{
    struct buf *buf = bread(log.dev, log.start);
    struct logheader *lh = (struct logheader *)(buf->data);
    int i;
    log.lh.n = lh->n;
    for (i = 0; i < log.lh.n; i++) {
        log.lh.block[i] = lh->block[i];
    }
    brelse(buf);
}

// メモリ上のログヘッダをディスクに書き込む。
// ここが現在のトランザクションが実際にコミットされる時点である。
static void write_head(void)
{
    struct buf *buf = bread(log.dev, log.start);
    struct logheader *hb = (struct logheader *)(buf->data);
    int i;
    hb->n = log.lh.n;
    for (i = 0; i < log.lh.n; i++) {
        hb->block[i] = log.lh.block[i];
    }
    bwrite(buf);
    brelse(buf);
}

static void recover_from_log(void)
{
    read_head();
    install_trans(1); // コミット済みならログからディスクへ複写する
    log.lh.n = 0;
    write_head(); // ログを消去する
}

// 各FSシステムコールの開始時に呼ばれる。
void begin_op(void)
{
    acquire(&log.lock);
    while (1) {
        if (log.committing) {
            sleep_prepare(&log);
            release(&log.lock);
            sleep();
            acquire(&log.lock);
        } else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGBLOCKS) {
            // この操作でログ領域を使い切る可能性があるため、コミットを待つ。
            sleep_prepare(&log);
            release(&log.lock);
            sleep();
            acquire(&log.lock);
        } else {
            log.outstanding += 1;
            release(&log.lock);
            break;
        }
    }
}

// 各FSシステムコールの終了時に呼ばれる。
// 実行中の最後の操作ならコミットする。
void end_op(void)
{
    int do_commit = 0;

    acquire(&log.lock);
    log.outstanding -= 1;
    if (log.committing)
        panic("log.committing");
    if (log.outstanding == 0) {
        do_commit = 1;
        log.committing = 1;
    } else {
        // begin_op()がログ領域を待っている可能性があり、
        // log.outstandingを減らすと予約済み領域が減る。
        wakeup(&log);
    }
    release(&log.lock);

    if (do_commit) {
        // ロックを保持したままスリープできないため、
        // ロックを保持せずにcommitを呼ぶ。
        commit();
        acquire(&log.lock);
        log.committing = 0;
        log.ncommit += 1;
        wakeup(&log);
        release(&log.lock);
    }
}

// 変更されたブロックをキャッシュからログへ複写する。
static void write_log(void)
{
    int tail;

    for (tail = 0; tail < log.lh.n; tail++) {
        struct buf *to = bread(log.dev, log.start + tail + 1); // ログブロック
        struct buf *from =
            bread(log.dev, log.lh.block[tail]); // キャッシュブロック
        memmove(to->data, from->data, BSIZE);
        bwrite(to); // ログを書き込む
        brelse(from);
        brelse(to);
    }
}

static void commit()
{
    if (log.lh.n > 0) {
        write_log();      // 変更ブロックをキャッシュからログへ書く
        write_head();     // ヘッダをディスクへ書く -- 実際のコミット
        install_trans(0); // 本来の場所へ書き込みを反映する
        log.lh.n = 0;
        write_head(); // ログからトランザクションを消去する
    }
}

// 呼び出し元はb->dataを変更し、バッファの使用を終えている。
// refcntを増やしてブロック番号を記録し、キャッシュ内でピン留めする。
// ディスクへの書き込みはcommit()/write_log()が行う。
//
// log_write()はbwrite()の代わりになる。典型的な使い方:
//   bp = bread(...)
//   bp->data[]を変更
//   log_write(bp)
//   brelse(bp)
void log_write(struct buf *b)
{
    int i;

    acquire(&log.lock);
    if (log.lh.n >= LOGBLOCKS)
        panic("too big a transaction");
    if (log.outstanding < 1)
        panic("log_write outside of trans");

    for (i = 0; i < log.lh.n; i++) {
        if (log.lh.block[i] == b->blockno) // ログの吸収
            break;
    }
    log.lh.block[i] = b->blockno;
    if (i == log.lh.n) { // ログに新しいブロックを追加する?
        bpin(b);
        log.lh.n++;
    }
    release(&log.lock);
}

uint64 sys_sync(void)
{
    acquire(&log.lock);
    if (log.committing || log.outstanding > 0) {
        int n = log.ncommit + 1;
        while (log.ncommit < n) {
            sleep_prepare(&log);
            release(&log.lock);
            sleep();
            acquire(&log.lock);
        }
    }
    release(&log.lock);
    return 0;
}
