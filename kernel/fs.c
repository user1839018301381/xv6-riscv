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
struct superblock sb;

// スーパーブロックを読み込む。
static void readsb(int dev, struct superblock *sb)
{
    struct buf *bp;

    bp = bread(dev, 1);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

// ファイルシステムを初期化する。
void fsinit(int dev)
{
    readsb(dev, &sb);
    if (sb.magic != FSMAGIC)
        panic("invalid file system");
    initlog(dev, &sb);
    ireclaim(dev);
}

// ブロックをゼロクリアする。
static void bzero(int dev, int bno)
{
    struct buf *bp;

    bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    log_write(bp);
    brelse(bp);
}

// ブロック。

// ゼロクリアされたディスクブロックを割り当てる。
// ディスク容量不足なら0を返す。
static uint balloc(uint dev)
{
    int b, bi, m;
    struct buf *bp;

    bp = 0;
    for (b = 0; b < sb.size; b += BPB) {
        bp = bread(dev, BBLOCK(b, sb));
        for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
            m = 1 << (bi % 8);
            if ((bp->data[bi / 8] & m) == 0) { // ブロックは空いているか?
                bp->data[bi / 8] |= m;         // ブロックを使用中にする。
                log_write(bp);
                brelse(bp);
                bzero(dev, b + bi);
                return b + bi;
            }
        }
        brelse(bp);
    }
    printk("balloc: out of blocks\n");
    return 0;
}

// ディスクブロックを解放する。
static void bfree(int dev, uint b)
{
    struct buf *bp;
    int bi, m;

    bp = bread(dev, BBLOCK(b, sb));
    bi = b % BPB;
    m = 1 << (bi % 8);
    if ((bp->data[bi / 8] & m) == 0)
        panic("freeing free block");
    bp->data[bi / 8] &= ~m;
    log_write(bp);
    brelse(bp);
}

// inode。
//
// inodeは名前のない1つのファイルを表す。
// inodeのディスク構造は、ファイル種別、サイズ、
// それを参照するリンク数、ファイル内容を保持するブロック一覧という
// メタデータを持つ。
//
// inodeはsb.inodestartのブロックからディスク上に連続して配置される。
// 各inodeはディスク上の位置を示す番号を持つ。
//
// カーネルは使用中inodeの表をメモリに保持し、
// 複数プロセスが使うinodeへのアクセスを同期する場所を提供する。
// メモリ上のinodeには、ディスクに保存されない管理情報
// ip->refとip->validも含まれる。
//
// inodeとそのメモリ上の表現は、残りのファイルシステムコードで
// 使えるようになるまで一連の状態を経る。
//
// * 割り当て: ディスク上のtypeが0以外ならinodeは割り当て済みである。
//   ialloc()が割り当て、参照数とリンク数が0になればiput()が解放する。
//
// * 表での参照: inode表のエントリはip->refが0なら空きである。
//   それ以外ではip->refが、そのエントリへのメモリ上のポインタ
//   （オープン中のファイルと現在のディレクトリ）の数を追跡する。
//   iget()は表のエントリを検索または作成してrefを増やし、iput()は減らす。
//
// * 有効: inode表エントリの情報（type、sizeなど）はip->validが1のときだけ正しい。
//   ilock()はディスクからinodeを読み込んでip->validを設定し、
//   iput()はip->refが0になったときip->validをクリアする。
//
// * ロック済み: ファイルシステムコードは、最初にinodeをロックしてからでなければ
//   inodeの情報や内容を調べたり変更したりできない。
//
// したがって典型的な手順は次のとおり:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... ip->xxxを調べて変更 ...
//   iunlock(ip)
//   iput(ip)
//
// ilock()をiget()から分けることで、システムコールはinodeへの長期参照
// （オープン中のファイルなど）を得たうえで、短時間だけロックできる
// （read()など）。この分離はパス名検索中のデッドロックや競合の回避にも役立つ。
// iget()はip->refを増やすので、inodeは表に残り、そのポインタは有効であり続ける。
//
// 多くの内部ファイルシステム関数は、関係するinodeを呼び出し元が
// ロック済みであることを期待する。これにより呼び出し元は
// 複数段階のアトミックな操作を作れる。
//
// itable.lockスピンロックはitableエントリの割り当てを保護する。
// ip->refはエントリが空きかを示し、ip->devとip->inumはエントリが
// 保持するinodeを示すため、これらのフィールドを使う間はitable.lockを保持する。
//
// ip->lockスリープロックはref、dev、inum以外のip->フィールドをすべて保護する。
// inodeのip->valid、ip->size、ip->typeなどを読むか書くにはip->lockを保持する。

struct {
    struct spinlock lock;
    struct inode inode[NINODE];
} itable;

void iinit()
{
    int i = 0;

    initlock(&itable.lock, "itable");
    for (i = 0; i < NINODE; i++) {
        initsleeplock(&itable.inode[i].lock, "inode");
    }
}

static struct inode *iget(uint dev, uint inum);

// デバイスdev上にinodeを割り当てる。
// typeを設定して割り当て済みとする。
// ロックされていないが割り当て済みで参照されているinodeを返し、
// 空きinodeがなければNULLを返す。
struct inode *ialloc(uint dev, short type)
{
    int inum;
    struct buf *bp;
    struct dinode *dip;

    for (inum = 1; inum < sb.ninodes; inum++) {
        bp = bread(dev, IBLOCK(inum, sb));
        dip = (struct dinode *)bp->data + inum % IPB;
        if (dip->type == 0) { // 空きinode
            memset(dip, 0, sizeof(*dip));
            dip->type = type;
            log_write(bp); // ディスク上で割り当て済みにする
            brelse(bp);
            return iget(dev, inum);
        }
        brelse(bp);
    }
    printk("ialloc: no inodes\n");
    return 0;
}

// 変更されたメモリ上のinodeをディスクへ複写する。
// ディスク上にも存在するip->xxxフィールドを変更するたびに呼ぶ。
// 呼び出し元はip->lockを保持していなければならない。
void iupdate(struct inode *ip)
{
    struct buf *bp;
    struct dinode *dip;

    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode *)bp->data + ip->inum % IPB;
    dip->type = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink;
    dip->size = ip->size;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
    log_write(bp);
    brelse(bp);
}

// デバイスdev上で番号inumのinodeを探し、メモリ上の複製を返す。
// inodeをロックせず、ディスクからも読み込まない。
static struct inode *iget(uint dev, uint inum)
{
    struct inode *ip, *empty;

    acquire(&itable.lock);

    // inodeはすでに表にあるか?
    empty = 0;
    for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;
            release(&itable.lock);
            return ip;
        }
        if (empty == 0 && ip->ref == 0) // 空きスロットを記録する。
            empty = ip;
    }

    // inodeエントリを再利用する。
    if (empty == 0)
        panic("iget: no inodes");

    ip = empty;
    ip->dev = dev;
    ip->inum = inum;
    ip->ref = 1;
    ip->valid = 0;
    release(&itable.lock);

    return ip;
}

// ipの参照カウントを増やす。
// ip = idup(ip1)という書き方ができるようipを返す。
struct inode *idup(struct inode *ip)
{
    acquire(&itable.lock);
    ip->ref++;
    release(&itable.lock);
    return ip;
}

// 指定されたinodeをロックする。
// 必要ならディスクからinodeを読み込む。
void ilock(struct inode *ip)
{
    struct buf *bp;
    struct dinode *dip;

    if (ip == 0 || ip->ref < 1)
        panic("ilock");

    acquiresleep(&ip->lock);

    if (ip->valid == 0) {
        bp = bread(ip->dev, IBLOCK(ip->inum, sb));
        dip = (struct dinode *)bp->data + ip->inum % IPB;
        ip->type = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
        brelse(bp);
        ip->valid = 1;
        if (ip->type == 0)
            panic("ilock: no type");
    }
}

// 指定されたinodeのロックを解除する。
void iunlock(struct inode *ip)
{
    if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
        panic("iunlock");

    releasesleep(&ip->lock);
}

// ディスク上のinodeを空きにする。
static void ifree(uint dev, uint inum)
{
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    dip->type = 0;
    log_write(bp);
    brelse(bp);
}

// メモリ上のinodeへの参照を1つ減らす。
// 最後の参照ならinode表のエントリを再利用できる。
// 最後の参照で、かつinodeへのリンクがなければ、ディスク上のinode（と内容）を解放する。
// inodeを解放する可能性があるため、iput()の呼び出しはすべてトランザクション内で行う。
void iput(struct inode *ip)
{
    acquire(&itable.lock);

    // リンクされていないinodeへの最後の参照か?
    // ref--の前にdev/inumを保存する。refが0になると、並行するiget()が
    // 別のinum用にipを再利用する可能性があるためである。
    int last = (ip->ref == 1 && ip->valid && ip->nlink == 0);
    uint dev = ip->dev, inum = ip->inum;

    if (last) {
        // ip->ref == 1なら、他のプロセスがipをロックしていることはない。
        acquiresleep(&ip->lock);
        release(&itable.lock);

        itrunc(ip); // データブロックを解放する（ディスク上のtypeは0以外のまま）
        ip->valid = 0;

        releasesleep(&ip->lock);

        acquire(&itable.lock);
    }

    ip->ref--;
    release(&itable.lock);

    if (last)
        ifree(dev, inum); // ディスク上のtypeを消し、inumを割り当て可能にする
}

// よく使う書き方: ロックを解除してからputする。
void iunlockput(struct inode *ip)
{
    iunlock(ip);
    iput(ip);
}

void ireclaim(int dev)
{
    for (int inum = 1; inum < sb.ninodes; inum++) {
        struct inode *ip = 0;
        struct buf *bp = bread(dev, IBLOCK(inum, sb));
        struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
        if (dip->type != 0 && dip->nlink == 0) { // 孤児inodeか
            printk("ireclaim: orphaned inode %d\n", inum);
            ip = iget(dev, inum);
        }
        brelse(bp);
        if (ip) {
            begin_op();
            ilock(ip);
            iunlock(ip);
            iput(ip);
            end_op();
        }
    }
}

// inodeの内容
//
// 各inodeに関連付けられた内容（データ）はディスク上のブロックに保存される。
// 最初のNDIRECT個のブロック番号はip->addrs[]に記録される。
// 次のNINDIRECT個はip->addrs[NDIRECT]のブロックに記録される。

// inode ipのn番目のブロックのディスクアドレスを返す。
// そのブロックがなければbmapが割り当てる。
// ディスク容量不足なら0を返す。
static uint bmap(struct inode *ip, uint bn)
{
    uint addr, *a;
    struct buf *bp;

    if (bn < NDIRECT) {
        if ((addr = ip->addrs[bn]) == 0) {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[bn] = addr;
        }
        return addr;
    }
    bn -= NDIRECT;

    if (bn < NINDIRECT) {
        // 必要なら間接ブロックを割り当てて読み込む。
        if ((addr = ip->addrs[NDIRECT]) == 0) {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[NDIRECT] = addr;
        }
        bp = bread(ip->dev, addr);
        a = (uint *)bp->data;
        if ((addr = a[bn]) == 0) {
            addr = balloc(ip->dev);
            if (addr) {
                a[bn] = addr;
                log_write(bp);
            }
        }
        brelse(bp);
        return addr;
    }

    panic("bmap: out of range");
}

// inodeを切り詰める（内容を破棄する）。
// 呼び出し元はip->lockを保持していなければならない。
void itrunc(struct inode *ip)
{
    int i, j;
    struct buf *bp;
    uint *a;

    for (i = 0; i < NDIRECT; i++) {
        if (ip->addrs[i]) {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    if (ip->addrs[NDIRECT]) {
        bp = bread(ip->dev, ip->addrs[NDIRECT]);
        a = (uint *)bp->data;
        for (j = 0; j < NINDIRECT; j++) {
            if (a[j])
                bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0;
    iupdate(ip);
}

// inodeからstat情報を複写する。
// 呼び出し元はip->lockを保持していなければならない。
void stati(struct inode *ip, struct stat *st)
{
    st->dev = ip->dev;
    st->ino = ip->inum;
    st->type = ip->type;
    st->nlink = ip->nlink;
    st->size = ip->size;
}

// inodeからデータを読み込む。
// 呼び出し元はip->lockを保持していなければならない。
// user_dst==1ならdstはユーザ仮想アドレス、それ以外ならカーネルアドレスである。
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
    uint tot, m;
    struct buf *bp;

    if (off > ip->size || off + n < off)
        return 0;
    if (off + n > ip->size)
        n = ip->size - off;

    for (tot = 0; tot < n; tot += m, off += m, dst += m) {
        uint addr = bmap(ip, off / BSIZE);
        if (addr == 0)
            break;
        bp = bread(ip->dev, addr);
        m = min(n - tot, BSIZE - off % BSIZE);
        if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
            brelse(bp);
            tot = -1;
            break;
        }
        brelse(bp);
    }
    return tot;
}

// inodeへデータを書き込む。
// 呼び出し元はip->lockを保持していなければならない。
// user_src==1ならsrcはユーザ仮想アドレス、それ以外ならカーネルアドレスである。
// 正常に書き込めたバイト数を返す。
// 戻り値が要求したnより小さければ何らかのエラーがある。
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
    uint tot, m;
    struct buf *bp;

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (tot = 0; tot < n; tot += m, off += m, src += m) {
        uint addr = bmap(ip, off / BSIZE);
        if (addr == 0)
            break;
        bp = bread(ip->dev, addr);
        m = min(n - tot, BSIZE - off % BSIZE);
        if (either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
            // ブロックを部分的に更新した可能性があるため、ログに記録する必要がある。
            log_write(bp);
            brelse(bp);
            break;
        }
        log_write(bp);
        brelse(bp);
    }

    if (off > ip->size)
        ip->size = off;

    // サイズが変わらなくてもinodeをディスクへ書き戻す。
    // 上のループがbmap()を呼んでip->addrs[]に新しいブロックを追加した可能性があるため。
    iupdate(ip);

    return tot;
}

// ディレクトリ

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

// ディレクトリ内でディレクトリエントリを探す。
// 見つかったら*poffにエントリのバイトオフセットを設定する。
struct inode *dirlookup(struct inode *dp, char *name, uint *poff)
{
    uint off, inum;
    struct dirent de;

    if (dp->type != T_DIR)
        panic("dirlookup not DIR");

    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");
        if (de.inum == 0)
            continue;
        if (namecmp(name, de.name) == 0) {
            // エントリがパス要素に一致する
            if (poff)
                *poff = off;
            inum = de.inum;
            return iget(dp->dev, inum);
        }
    }

    return 0;
}

// 新しいディレクトリエントリ(name, inum)をディレクトリdpへ書き込む。
// 成功時は0、失敗時（ディスクブロック不足など）は-1を返す。
int dirlink(struct inode *dp, char *name, uint inum)
{
    int off;
    struct dirent de;
    struct inode *ip;

    // nameが存在しないことを確認する。
    if ((ip = dirlookup(dp, name, 0)) != 0) {
        iput(ip);
        return -1;
    }

    // 空のdirentを探す。
    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        if (de.inum == 0)
            break;
    }

    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
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
static char *skipelem(char *path, char *name)
{
    char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// パス名を検索してinodeを返す。
// parent != 0なら親のinodeを返し、最後のパス要素をnameへ複写する。
// nameにはDIRSIZバイトの領域が必要である。
// iput()を呼ぶため、トランザクション内で呼ばなければならない。
static struct inode *namex(char *path, int nameiparent, char *name)
{
    struct inode *ip, *next;

    if (*path == '/')
        ip = iget(ROOTDEV, ROOTINO);
    else
        ip = idup(myproc()->cwd);

    while ((path = skipelem(path, name)) != 0) {
        ilock(ip);
        if (ip->type != T_DIR) {
            iunlockput(ip);
            return 0;
        }
        if (ip->nlink == 0) {
            iunlockput(ip);
            return 0;
        }
        if (nameiparent && *path == '\0') {
            // 1レベル手前で止める。
            iunlock(ip);
            return ip;
        }
        if ((next = dirlookup(ip, name, 0)) == 0) {
            iunlockput(ip);
            return 0;
        }
        iunlockput(ip);
        ip = next;
    }
    if (nameiparent) {
        iput(ip);
        return 0;
    }
    return ip;
}

struct inode *namei(char *path)
{
    char name[DIRSIZ];
    return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name)
{
    return namex(path, 1, name);
}
