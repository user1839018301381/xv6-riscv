// ファイルシステムの実装。5つの層からなる:
//   + ブロック: 生のディスクブロックのアロケータ。
//   + ログ: 複数段階の更新に対するクラッシュ復旧。
//   + ファイル: inodeの割り当て、読み書き、メタデータ。
//   + ディレクトリ: 特別な内容（他のinodeの一覧）を持つinode。
//   + 名前: 分かりやすい名前を付けるための/usr/rtm/xv6/fs.cのようなパス。
//
// このファイルには低レベルのファイルシステム操作ルーチンが含まれる。
// より高レベルなシステムコールの実装はsysfile.cにある。

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// ディスクデバイスごとにスーパーブロックが1つあるべきだが、
// ここでは1つのデバイスだけを使う。
struct superblock filesystem_superblock;

// スーパーブロックを読み込む。
static void readsb(int device, struct superblock *superblock_out)
{
    struct buf *buffer;

    buffer = bread(device, 1);
    memmove(superblock_out, buffer->block_data, sizeof(*superblock_out));
    brelse(buffer);
}

// ファイルシステムを初期化する。
void fsinit(int device)
{
    readsb(device, &filesystem_superblock);
    if (filesystem_superblock.magic != FSMAGIC)
        panic("invalid file system");
    initlog(device, &filesystem_superblock);
    ireclaim(device);
}

// ブロックをゼロクリアする。
static void bzero(int device, int block_number)
{
    struct buf *buffer;

    buffer = bread(device, block_number);
    memset(buffer->block_data, 0, BSIZE);
    log_write(buffer);
    brelse(buffer);
}

// ブロック。

// ゼロクリアされたディスクブロックを割り当てる。
// ディスク容量不足なら0を返す。
static uint balloc(uint device)
{
    int base_block, bitmap_index, bitmap_mask;
    struct buf *buffer;

    buffer = 0;
    for (base_block = 0;
         base_block < filesystem_superblock.total_block_count;
         base_block += BPB) {
        buffer = bread(device,
                       BBLOCK(base_block, filesystem_superblock));
        for (bitmap_index = 0;
             bitmap_index < BPB &&
             base_block + bitmap_index <
                 filesystem_superblock.total_block_count;
             bitmap_index++) {
            bitmap_mask = 1 << (bitmap_index % 8);
            if ((buffer->block_data[bitmap_index / 8] & bitmap_mask) == 0) {
                buffer->block_data[bitmap_index / 8] |= bitmap_mask;
                log_write(buffer);
                brelse(buffer);
                bzero(device, base_block + bitmap_index);
                return base_block + bitmap_index;
            }
        }
        brelse(buffer);
    }
    printk("balloc: out of blocks\n");
    return 0;
}

// ディスクブロックを解放する。
static void bfree(int device, uint block_number)
{
    struct buf *buffer;
    int bitmap_index, bitmap_mask;

    buffer = bread(device, BBLOCK(block_number, filesystem_superblock));
    bitmap_index = block_number % BPB;
    bitmap_mask = 1 << (bitmap_index % 8);
    if ((buffer->block_data[bitmap_index / 8] & bitmap_mask) == 0)
        panic("freeing free block");
    buffer->block_data[bitmap_index / 8] &= ~bitmap_mask;
    log_write(buffer);
    brelse(buffer);
}

// inode。
//
// inodeは名前のない1つのファイルを表す。
// inodeのディスク構造は、ファイル種別、サイズ、
// それを参照するリンク数、ファイル内容を保持するブロック一覧という
// メタデータを持つ。
//
// inodeはfilesystem_superblock.inodestartのブロックからディスク上に連続して配置される。
// 各inodeはディスク上の位置を示す番号を持つ。
//
// カーネルは使用中inodeの表をメモリに保持し、
// 複数プロセスが使うinodeへのアクセスを同期する場所を提供する。
// メモリ上のinodeには、ディスクに保存されない管理情報
// inode->reference_countとinode->is_validも含まれる。
//
// inodeとそのメモリ上の表現は、残りのファイルシステムコードで
// 使えるようになるまで一連の状態を経る。
//
// * 割り当て: ディスク上のtypeが0以外ならinodeは割り当て済みである。
//   ialloc()が割り当て、参照数とリンク数が0になればiput()が解放する。
//
// * 表での参照: inode表のエントリはinode->reference_countが0なら空きである。
//   それ以外ではinode->reference_countが、そのエントリへのメモリ上のポインタ
//   （オープン中のファイルと現在のディレクトリ）の数を追跡する。
//   iget()は表のエントリを検索または作成してrefを増やし、iput()は減らす。
//
// * 有効: inode表エントリの情報（type、sizeなど）はinode->is_validが1のときだけ正しい。
//   ilock()はディスクからinodeを読み込んでinode->is_validを設定し、
//   iput()はinode->reference_countが0になったときinode->is_validをクリアする。
//
// * ロック済み: ファイルシステムコードは、最初にinodeをロックしてからでなければ
//   inodeの情報や内容を調べたり変更したりできない。
//
// したがって典型的な手順は次のとおり:
//   inode = iget(dev, inum)
//   ilock(inode)
//   ... inode->xxxを調べて変更 ...
//   iunlock(inode)
//   iput(inode)
//
// ilock()をiget()から分けることで、システムコールはinodeへの長期参照
// （オープン中のファイルなど）を得たうえで、短時間だけロックできる
// （read()など）。この分離はパス名検索中のデッドロックや競合の回避にも役立つ。
// iget()はinode->reference_countを増やすので、inodeは表に残り、そのポインタは有効であり続ける。
//
// 多くの内部ファイルシステム関数は、関係するinodeを呼び出し元が
// ロック済みであることを期待する。これにより呼び出し元は
// 複数段階のアトミックな操作を作れる。
//
// inode_table.lockスピンロックはinode_tableエントリの割り当てを保護する。
// inode->reference_countはエントリが空きかを示し、inode->deviceとinode->inode_numberはエントリが
// 保持するinodeを示すため、これらのフィールドを使う間はinode_table.lockを保持する。
//
// inode->lockスリープロックはref、dev、inum以外のinode->フィールドをすべて保護する。
// inode->is_valid、inode->size、inode->typeなどを読むか書くにはinode->lockを保持する。

struct {
    struct spinlock lock;
    struct inode inodes[NINODE];
} inode_table;

void iinit()
{
    int i = 0;

    initlock(&inode_table.lock, "inode_table");
    for (i = 0; i < NINODE; i++) {
        initsleeplock(&inode_table.inodes[i].lock, "inode");
    }
}

static struct inode *iget(uint device, uint inode_number);

// デバイスdev上にinodeを割り当てる。
// typeを設定して割り当て済みとする。
// ロックされていないが割り当て済みで参照されているinodeを返し、
// 空きinodeがなければNULLを返す。
struct inode *ialloc(uint device, short inode_type)
{
    int inode_number;
    struct buf *buffer;
    struct dinode *disk_inode;

    for (inode_number = 1;
         inode_number < filesystem_superblock.inode_count; inode_number++) {
        buffer = bread(device,
                       IBLOCK(inode_number, filesystem_superblock));
        disk_inode = (struct dinode *)buffer->block_data + inode_number % IPB;
        if (disk_inode->type == 0) { // 空きinode
            memset(disk_inode, 0, sizeof(*disk_inode));
            disk_inode->type = inode_type;
            log_write(buffer); // ディスク上で割り当て済みにする
            brelse(buffer);
            return iget(device, inode_number);
        }
        brelse(buffer);
    }
    printk("ialloc: no inodes\n");
    return 0;
}

// 変更されたメモリ上のinodeをディスクへ複写する。
// ディスク上にも存在するinode->xxxフィールドを変更するたびに呼ぶ。
// 呼び出し元はinode->lockを保持していなければならない。
void iupdate(struct inode *inode)
{
    struct buf *buffer;
    struct dinode *disk_inode;

    buffer = bread(inode->device, IBLOCK(inode->inode_number, filesystem_superblock));
    disk_inode = (struct dinode *)buffer->block_data + inode->inode_number % IPB;
    disk_inode->type = inode->type;
    disk_inode->major = inode->major;
    disk_inode->minor = inode->minor;
    disk_inode->link_count = inode->link_count;
    disk_inode->size = inode->size;
    memmove(disk_inode->block_addresses, inode->block_addresses, sizeof(inode->block_addresses));
    log_write(buffer);
    brelse(buffer);
}

// デバイスdev上で番号inumのinodeを探し、メモリ上の複製を返す。
// inodeをロックせず、ディスクからも読み込まない。
static struct inode *iget(uint device, uint inode_number)
{
    struct inode *inode, *empty_slot;

    acquire(&inode_table.lock);

    // inodeはすでに表にあるか?
    empty_slot = 0;
    for (inode = &inode_table.inodes[0];
         inode < &inode_table.inodes[NINODE]; inode++) {
        if (inode->reference_count > 0 && inode->device == device &&
            inode->inode_number == inode_number) {
            inode->reference_count++;
            release(&inode_table.lock);
            return inode;
        }
        if (empty_slot == 0 && inode->reference_count == 0)
            empty_slot = inode;
    }

    // inodeエントリを再利用する。
    if (empty_slot == 0)
        panic("iget: no inodes");

    inode = empty_slot;
    inode->device = device;
    inode->inode_number = inode_number;
    inode->reference_count = 1;
    inode->is_valid = 0;
    release(&inode_table.lock);

    return inode;
}

// inodeの参照カウントを増やす。
// inode = idup(ip1)という書き方ができるようinodeを返す。
struct inode *idup(struct inode *inode)
{
    acquire(&inode_table.lock);
    inode->reference_count++;
    release(&inode_table.lock);
    return inode;
}

// 指定されたinodeをロックする。
// 必要ならディスクからinodeを読み込む。
void ilock(struct inode *inode)
{
    struct buf *buffer;
    struct dinode *disk_inode;

    if (inode == 0 || inode->reference_count < 1)
        panic("ilock");

    acquiresleep(&inode->lock);

    if (inode->is_valid == 0) {
        buffer = bread(inode->device, IBLOCK(inode->inode_number, filesystem_superblock));
        disk_inode = (struct dinode *)buffer->block_data + inode->inode_number % IPB;
        inode->type = disk_inode->type;
        inode->major = disk_inode->major;
        inode->minor = disk_inode->minor;
        inode->link_count = disk_inode->link_count;
        inode->size = disk_inode->size;
        memmove(inode->block_addresses, disk_inode->block_addresses, sizeof(inode->block_addresses));
        brelse(buffer);
        inode->is_valid = 1;
        if (inode->type == 0)
            panic("ilock: no type");
    }
}

// 指定されたinodeのロックを解除する。
void iunlock(struct inode *inode)
{
    if (inode == 0 || !holdingsleep(&inode->lock) || inode->reference_count < 1)
        panic("iunlock");

    releasesleep(&inode->lock);
}

// ディスク上のinodeを空きにする。
static void ifree(uint device, uint inode_number)
{
    struct buf *buffer =
        bread(device, IBLOCK(inode_number, filesystem_superblock));
    struct dinode *disk_inode =
        (struct dinode *)buffer->block_data + inode_number % IPB;
    disk_inode->type = 0;
    log_write(buffer);
    brelse(buffer);
}

// メモリ上のinodeへの参照を1つ減らす。
// 最後の参照ならinode表のエントリを再利用できる。
// 最後の参照で、かつinodeへのリンクがなければ、ディスク上のinode（と内容）を解放する。
// inodeを解放する可能性があるため、iput()の呼び出しはすべてトランザクション内で行う。
void iput(struct inode *inode)
{
    acquire(&inode_table.lock);

    // リンクされていないinodeへの最後の参照か?
    // ref--の前にdev/inumを保存する。refが0になると、並行するiget()が
    // 別のinum用にinodeを再利用する可能性があるためである。
    int is_last_reference =
        (inode->reference_count == 1 && inode->is_valid && inode->link_count == 0);
    uint device = inode->device, inode_number = inode->inode_number;

    if (is_last_reference) {
        // inode->reference_count == 1なら、他のプロセスがinodeをロックしていることはない。
        acquiresleep(&inode->lock);
        release(&inode_table.lock);

        itrunc(inode); // データブロックを解放する（ディスク上のtypeは0以外のまま）
        inode->is_valid = 0;

        releasesleep(&inode->lock);

        acquire(&inode_table.lock);
    }

    inode->reference_count--;
    release(&inode_table.lock);

    if (is_last_reference)
        ifree(device, inode_number);
}

// よく使う書き方: ロックを解除してからputする。
void iunlockput(struct inode *inode)
{
    iunlock(inode);
    iput(inode);
}

void ireclaim(int device)
{
    for (int inode_number = 1;
         inode_number < filesystem_superblock.inode_count; inode_number++) {
        struct inode *inode = 0;
        struct buf *buffer =
            bread(device, IBLOCK(inode_number, filesystem_superblock));
        struct dinode *disk_inode =
            (struct dinode *)buffer->block_data + inode_number % IPB;
        if (disk_inode->type != 0 && disk_inode->link_count == 0) { // 孤児inodeか
            printk("ireclaim: orphaned inode %d\n", inode_number);
            inode = iget(device, inode_number);
        }
        brelse(buffer);
        if (inode) {
            begin_op();
            ilock(inode);
            iunlock(inode);
            iput(inode);
            end_op();
        }
    }
}

// inodeの内容
//
// 各inodeに関連付けられた内容（データ）はディスク上のブロックに保存される。
// 最初のNDIRECT個のブロック番号はinode->block_addresses[]に記録される。
// 次のNINDIRECT個はinode->block_addresses[NDIRECT]のブロックに記録される。

// inodeのblock_index番目のブロックのディスクアドレスを返す。
// そのブロックがなければbmapが割り当てる。
// ディスク容量不足なら0を返す。
static uint bmap(struct inode *inode, uint block_index)
{
    uint block_address, *indirect_addresses;
    struct buf *buffer;

    if (block_index < NDIRECT) {
        if ((block_address = inode->block_addresses[block_index]) == 0) {
            block_address = balloc(inode->device);
            if (block_address == 0)
                return 0;
            inode->block_addresses[block_index] = block_address;
        }
        return block_address;
    }
    block_index -= NDIRECT;

    if (block_index < NINDIRECT) {
        // 必要なら間接ブロックを割り当てて読み込む。
        if ((block_address = inode->block_addresses[NDIRECT]) == 0) {
            block_address = balloc(inode->device);
            if (block_address == 0)
                return 0;
            inode->block_addresses[NDIRECT] = block_address;
        }
        buffer = bread(inode->device, block_address);
        indirect_addresses = (uint *)buffer->block_data;
        if ((block_address = indirect_addresses[block_index]) == 0) {
            block_address = balloc(inode->device);
            if (block_address) {
                indirect_addresses[block_index] = block_address;
                log_write(buffer);
            }
        }
        brelse(buffer);
        return block_address;
    }

    panic("bmap: out of range");
}

// inodeを切り詰める（内容を破棄する）。
// 呼び出し元はinode->lockを保持していなければならない。
void itrunc(struct inode *inode)
{
    int i, j;
    struct buf *buffer;
    uint *indirect_addresses;

    for (i = 0; i < NDIRECT; i++) {
        if (inode->block_addresses[i]) {
            bfree(inode->device, inode->block_addresses[i]);
            inode->block_addresses[i] = 0;
        }
    }

    if (inode->block_addresses[NDIRECT]) {
        buffer = bread(inode->device, inode->block_addresses[NDIRECT]);
        indirect_addresses = (uint *)buffer->block_data;
        for (j = 0; j < NINDIRECT; j++) {
            if (indirect_addresses[j])
                bfree(inode->device, indirect_addresses[j]);
        }
        brelse(buffer);
        bfree(inode->device, inode->block_addresses[NDIRECT]);
        inode->block_addresses[NDIRECT] = 0;
    }

    inode->size = 0;
    iupdate(inode);
}

// inodeからstat情報を複写する。
// 呼び出し元はinode->lockを保持していなければならない。
void stati(struct inode *inode, struct stat *file_status)
{
    file_status->device = inode->device;
    file_status->inode_number = inode->inode_number;
    file_status->type = inode->type;
    file_status->link_count = inode->link_count;
    file_status->size = inode->size;
}

// inodeからデータを読み込む。
// 呼び出し元はinode->lockを保持していなければならない。
// destination_is_userなら複写先はユーザ仮想アドレスである。
int readi(struct inode *inode, int destination_is_user,
          uint64 destination_address, uint offset, uint byte_count)
{
    uint total_bytes_read, chunk_byte_count;
    struct buf *buffer;

    if (offset > inode->size || offset + byte_count < offset)
        return 0;
    if (offset + byte_count > inode->size)
        byte_count = inode->size - offset;

    for (total_bytes_read = 0; total_bytes_read < byte_count;
         total_bytes_read += chunk_byte_count, offset += chunk_byte_count,
         destination_address += chunk_byte_count) {
        uint block_address = bmap(inode, offset / BSIZE);
        if (block_address == 0)
            break;
        buffer = bread(inode->device, block_address);
        chunk_byte_count = min(byte_count - total_bytes_read,
                               BSIZE - offset % BSIZE);
        if (either_copyout(destination_is_user, destination_address,
                           buffer->block_data + (offset % BSIZE),
                           chunk_byte_count) == -1) {
            brelse(buffer);
            total_bytes_read = -1;
            break;
        }
        brelse(buffer);
    }
    return total_bytes_read;
}

// inodeへデータを書き込む。
// 呼び出し元はinode->lockを保持していなければならない。
// source_is_userなら複写元はユーザ仮想アドレスである。
// 正常に書き込めたバイト数を返す。
// 戻り値が要求したbyte_countより小さければ何らかのエラーがある。
int writei(struct inode *inode, int source_is_user, uint64 source_address,
           uint offset, uint byte_count)
{
    uint total_bytes_written, chunk_byte_count;
    struct buf *buffer;

    if (offset > inode->size || offset + byte_count < offset)
        return -1;
    if (offset + byte_count > MAXFILE * BSIZE)
        return -1;

    for (total_bytes_written = 0; total_bytes_written < byte_count;
         total_bytes_written += chunk_byte_count, offset += chunk_byte_count,
         source_address += chunk_byte_count) {
        uint block_address = bmap(inode, offset / BSIZE);
        if (block_address == 0)
            break;
        buffer = bread(inode->device, block_address);
        chunk_byte_count = min(byte_count - total_bytes_written,
                               BSIZE - offset % BSIZE);
        if (either_copyin(buffer->block_data + (offset % BSIZE), source_is_user,
                          source_address, chunk_byte_count) == -1) {
            // ブロックを部分的に更新した可能性があるため、ログに記録する必要がある。
            log_write(buffer);
            brelse(buffer);
            break;
        }
        log_write(buffer);
        brelse(buffer);
    }

    if (offset > inode->size)
        inode->size = offset;

    // サイズが変わらなくてもinodeをディスクへ書き戻す。
    // 上のループがbmap()を呼んでinode->block_addresses[]に新しいブロックを追加した可能性があるため。
    iupdate(inode);

    return total_bytes_written;
}

// ディレクトリ

int namecmp(const char *left, const char *right)
{
    return strncmp(left, right, DIRSIZ);
}

// ディレクトリ内でディレクトリエントリを探す。
// 見つかったら*offset_outにエントリのバイトオフセットを設定する。
struct inode *dirlookup(struct inode *directory_inode, char *name,
                        uint *offset_out)
{
    uint entry_offset, inode_number;
    struct dirent entry;

    if (directory_inode->type != T_DIR)
        panic("dirlookup not DIR");

    for (entry_offset = 0; entry_offset < directory_inode->size;
         entry_offset += sizeof(entry)) {
        if (readi(directory_inode, 0, (uint64)&entry, entry_offset,
                  sizeof(entry)) != sizeof(entry))
            panic("dirlookup read");
        if (entry.inode_number == 0)
            continue;
        if (namecmp(name, entry.name) == 0) {
            // エントリがパス要素に一致する
            if (offset_out)
                *offset_out = entry_offset;
            inode_number = entry.inode_number;
            return iget(directory_inode->device, inode_number);
        }
    }

    return 0;
}

// 新しいディレクトリエントリをディレクトリへ書き込む。
// 成功時は0、失敗時（ディスクブロック不足など）は-1を返す。
int dirlink(struct inode *directory_inode, char *name, uint inode_number)
{
    int entry_offset;
    struct dirent entry;
    struct inode *existing_inode;

    // nameが存在しないことを確認する。
    if ((existing_inode = dirlookup(directory_inode, name, 0)) != 0) {
        iput(existing_inode);
        return -1;
    }

    // 空のdirentを探す。
    for (entry_offset = 0; entry_offset < directory_inode->size;
         entry_offset += sizeof(entry)) {
        if (readi(directory_inode, 0, (uint64)&entry, entry_offset,
                  sizeof(entry)) != sizeof(entry))
            panic("dirlink read");
        if (entry.inode_number == 0)
            break;
    }

    strncpy(entry.name, name, DIRSIZ);
    entry.inode_number = inode_number;
    if (writei(directory_inode, 0, (uint64)&entry, entry_offset,
               sizeof(entry)) != sizeof(entry))
        return -1;

    return 0;
}

// パス

// pathの次のパス要素をnameへ複写する。
// 複写した要素の次を指すポインタを返す。
// 返されるパスには先頭スラッシュがないため、呼び出し元は
// *path=='\0'を調べてnameが最後か確認できる。
// 削除する名前がなければ0を返す。
//
// 例:
//   skipelem("a/bb/c", name) = "bb/c", name = "a"に設定
//   skipelem("///a//bb", name) = "bb", name = "a"に設定
//   skipelem("a", name) = "", name = "a"に設定
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skip_path_element(char *path, char *name)
{
    char *element_start;
    int element_length;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    element_start = path;
    while (*path != '/' && *path != 0)
        path++;
    element_length = path - element_start;
    if (element_length >= DIRSIZ)
        memmove(name, element_start, DIRSIZ);
    else {
        memmove(name, element_start, element_length);
        name[element_length] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// パス名を検索してinodeを返す。
// parent != 0なら親のinodeを返し、最後のパス要素をnameへ複写する。
// nameにはDIRSIZバイトの領域が必要である。
// iput()を呼ぶため、トランザクション内で呼ばなければならない。
static struct inode *resolve_path_inode(char *path, int should_return_parent,
                                        char *name)
{
    struct inode *inode, *next_inode;

    if (*path == '/')
        inode = iget(ROOTDEV, ROOTINO);
    else
        inode = idup(myproc()->current_directory);

    while ((path = skip_path_element(path, name)) != 0) {
        ilock(inode);
        if (inode->type != T_DIR) {
            iunlockput(inode);
            return 0;
        }
        if (inode->link_count == 0) {
            iunlockput(inode);
            return 0;
        }
        if (should_return_parent && *path == '\0') {
            // 1レベル手前で止める。
            iunlock(inode);
            return inode;
        }
        if ((next_inode = dirlookup(inode, name, 0)) == 0) {
            iunlockput(inode);
            return 0;
        }
        iunlockput(inode);
        inode = next_inode;
    }
    if (should_return_parent) {
        iput(inode);
        return 0;
    }
    return inode;
}

struct inode *namei(char *path)
{
    char name[DIRSIZ];
    return resolve_path_inode(path, 0, name);
}

struct inode *nameiparent(char *path, char *name)
{
    return resolve_path_inode(path, 1, name);
}
