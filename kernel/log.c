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
    int block_count;
    int block_numbers[LOGBLOCKS];
};

struct log {
    struct spinlock lock;
    int start_block;
    int active_operations;
    int is_committing;
    int device;
    int commit_count;
    struct logheader header;
};
struct log log_state;

static void recover_from_log(void);
static void commit();

void initlog(int device, struct superblock *superblock)
{
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    initlock(&log_state.lock, "log_state");
    log_state.start_block = superblock->log_start_block;
    log_state.device = device;
    recover_from_log();
}

// コミット済みのブロックをログから本来の場所へ複写する。
static void install_transaction(int is_recovering)
{
    int log_index;

    for (log_index = 0; log_index < log_state.header.block_count;
         log_index++) {
        if (is_recovering) {
            printk("recovering log index %d destination %d\n", log_index,
                   log_state.header.block_numbers[log_index]);
        }
        struct buf *log_buffer =
            bread(log_state.device,
                  log_state.start_block + log_index + 1);
        struct buf *destination_buffer =
            bread(log_state.device,
                  log_state.header.block_numbers[log_index]);
        memmove(destination_buffer->block_data, log_buffer->block_data, BSIZE);
        bwrite(destination_buffer);
        if (!is_recovering)
            bunpin(destination_buffer);
        brelse(log_buffer);
        brelse(destination_buffer);
    }
}

// ディスク上のログヘッダをメモリ上のログヘッダに読み込む。
static void read_log_header(void)
{
    struct buf *buffer =
        bread(log_state.device, log_state.start_block);
    struct logheader *disk_header = (struct logheader *)(buffer->block_data);
    int i;
    log_state.header.block_count = disk_header->block_count;
    for (i = 0; i < log_state.header.block_count; i++) {
        log_state.header.block_numbers[i] = disk_header->block_numbers[i];
    }
    brelse(buffer);
}

// メモリ上のログヘッダをディスクに書き込む。
// ここが現在のトランザクションが実際にコミットされる時点である。
static void write_log_header(void)
{
    struct buf *buffer =
        bread(log_state.device, log_state.start_block);
    struct logheader *disk_header = (struct logheader *)(buffer->block_data);
    int i;
    disk_header->block_count = log_state.header.block_count;
    for (i = 0; i < log_state.header.block_count; i++) {
        disk_header->block_numbers[i] = log_state.header.block_numbers[i];
    }
    bwrite(buffer);
    brelse(buffer);
}

static void recover_from_log(void)
{
    read_log_header();
    install_transaction(1); // コミット済みならログからディスクへ複写する
    log_state.header.block_count = 0;
    write_log_header(); // ログを消去する
}

// 各FSシステムコールの開始時に呼ばれる。
void begin_op(void)
{
    acquire(&log_state.lock);
    while (1) {
        if (log_state.is_committing) {
            sleep_prepare(&log_state);
            release(&log_state.lock);
            sleep();
            acquire(&log_state.lock);
        } else if (log_state.header.block_count +
                       (log_state.active_operations + 1) * MAXOPBLOCKS >
                   LOGBLOCKS) {
            // この操作でログ領域を使い切る可能性があるため、コミットを待つ。
            sleep_prepare(&log_state);
            release(&log_state.lock);
            sleep();
            acquire(&log_state.lock);
        } else {
            log_state.active_operations++;
            release(&log_state.lock);
            break;
        }
    }
}

// 各FSシステムコールの終了時に呼ばれる。
// 実行中の最後の操作ならコミットする。
void end_op(void)
{
    int should_commit = 0;

    acquire(&log_state.lock);
    log_state.active_operations--;
    if (log_state.is_committing)
        panic("log_state.is_committing");
    if (log_state.active_operations == 0) {
        should_commit = 1;
        log_state.is_committing = 1;
    } else {
        // begin_op()がログ領域を待っている可能性があり、
        // active_operationsを減らすと予約済み領域が減る。
        wakeup(&log_state);
    }
    release(&log_state.lock);

    if (should_commit) {
        // ロックを保持したままスリープできないため、
        // ロックを保持せずにcommitを呼ぶ。
        commit();
        acquire(&log_state.lock);
        log_state.is_committing = 0;
        log_state.commit_count++;
        wakeup(&log_state);
        release(&log_state.lock);
    }
}

// 変更されたブロックをキャッシュからログへ複写する。
static void write_log(void)
{
    int log_index;

    for (log_index = 0; log_index < log_state.header.block_count;
         log_index++) {
        struct buf *log_buffer =
            bread(log_state.device,
                  log_state.start_block + log_index + 1);
        struct buf *cache_buffer =
            bread(log_state.device,
                  log_state.header.block_numbers[log_index]);
        memmove(log_buffer->block_data, cache_buffer->block_data, BSIZE);
        bwrite(log_buffer);
        brelse(cache_buffer);
        brelse(log_buffer);
    }
}

static void commit()
{
    if (log_state.header.block_count > 0) {
        write_log();      // 変更ブロックをキャッシュからログへ書く
        write_log_header();     // ヘッダをディスクへ書く -- 実際のコミット
        install_transaction(0); // 本来の場所へ書き込みを反映する
        log_state.header.block_count = 0;
        write_log_header(); // ログからトランザクションを消去する
    }
}

// 呼び出し元はb->block_dataを変更し、バッファの使用を終えている。
// refcntを増やしてブロック番号を記録し、キャッシュ内でピン留めする。
// ディスクへの書き込みはcommit()/write_log()が行う。
//
// log_write()はbwrite()の代わりになる。典型的な使い方:
//   bp = bread(...)
//   bp->block_data[]を変更
//   log_write(bp)
//   brelse(bp)
void log_write(struct buf *buffer)
{
    int i;

    acquire(&log_state.lock);
    if (log_state.header.block_count >= LOGBLOCKS)
        panic("too big a transaction");
    if (log_state.active_operations < 1)
        panic("log_write outside of trans");

    for (i = 0; i < log_state.header.block_count; i++) {
        if (log_state.header.block_numbers[i] == buffer->block_number)
            break;
    }
    log_state.header.block_numbers[i] = buffer->block_number;
    if (i == log_state.header.block_count) {
        bpin(buffer);
        log_state.header.block_count++;
    }
    release(&log_state.lock);
}

uint64 sys_sync(void)
{
    acquire(&log_state.lock);
    if (log_state.is_committing || log_state.active_operations > 0) {
        int target_commit_count = log_state.commit_count + 1;
        while (log_state.commit_count < target_commit_count) {
            sleep_prepare(&log_state);
            release(&log_state.lock);
            sleep();
            acquire(&log_state.lock);
        }
    }
    release(&log_state.lock);
    return 0;
}
