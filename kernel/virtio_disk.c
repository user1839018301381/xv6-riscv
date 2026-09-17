//
// qemuのvirtioディスクデバイス用ドライバ。
// qemuのvirtio MMIOインタフェースを使う。
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// virtio MMIOレジスタrのアドレス。
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk {
    // DMAディスクリプタの集合（リングではない）。
    // ドライバはこれで個々のディスク操作の読み書き場所をデバイスに伝える。
    // ディスクリプタはNUM個ある。
    // ほとんどのコマンドは、これらのディスクリプタ数個による
    // 「チェーン」（連結リスト）で構成される。
    struct virtq_desc *desc;

    // ドライバがデバイスに処理してほしいディスクリプタ番号を書くリング。
    // 各チェーンの先頭ディスクリプタだけを含み、NUM個の要素がある。
    struct virtq_avail *avail;

    // デバイスが処理を終えたディスクリプタ番号を書くリング
    // （各チェーンの先頭だけ）。usedリングエントリはNUM個ある。
    struct virtq_used *used;

    // ドライバ自身の管理情報。
    char free[NUM];  // ディスクリプタは空いているか?
    uint16 used_idx; // used[2..NUM]のここまで調べた。

    // 実行中の操作の情報を追跡する。
    // 完了割り込みが到着したときに使う。
    // チェーン先頭のディスクリプタ添字で索引する。
    struct {
        struct buf *b;
        char status;
    } info[NUM];

    // ディスクコマンドヘッダ。
    // 便利なようにディスクリプタと1対1で対応させる。
    struct virtio_blk_req ops[NUM];

    struct spinlock vdisk_lock;

} disk;

void virtio_disk_init(void)
{
    uint32 status = 0;

    initlock(&disk.vdisk_lock, "virtio_disk");

    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(VIRTIO_MMIO_VERSION) != 2 || *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
        panic("could not find virtio disk");
    }

    // デバイスをリセットする
    *R(VIRTIO_MMIO_STATUS) = status;

    // ACKNOWLEDGEステータスビットを設定する
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;

    // DRIVERステータスビットを設定する
    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 機能をネゴシエートする
    uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_FLUSH);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

    // 機能のネゴシエーションが完了したことをデバイスに伝える。
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // FEATURES_OKが設定されたことを確認するためステータスを再読する。
    status = *R(VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk FEATURES_OK unset");

    // キュー0を初期化する。
    *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

    // キュー0が使用中でないことを確認する。
    if (*R(VIRTIO_MMIO_QUEUE_READY))
        panic("virtio disk should not be ready");

    // キューの最大サイズを確認する。
    uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)
        panic("virtio disk has no queue 0");
    if (max < NUM)
        panic("virtio disk max queue too short");

    // キューメモリを割り当ててゼロクリアする。
    disk.desc = kalloc();
    disk.avail = kalloc();
    disk.used = kalloc();
    if (!disk.desc || !disk.avail || !disk.used)
        panic("virtio disk kalloc");
    memset(disk.desc, 0, PGSIZE);
    memset(disk.avail, 0, PGSIZE);
    memset(disk.used, 0, PGSIZE);

    // キューサイズを設定する。
    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    // 物理アドレスを書き込む。
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

    // キューの準備完了。
    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    // NUM個のディスクリプタはすべて未使用で開始する。
    for (int i = 0; i < NUM; i++)
        disk.free[i] = 1;

    // 完全に準備完了したことをデバイスに伝える。
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // plic.cとtrap.cがVIRTIO0_IRQからの割り込みを設定する。
}

// 空きディスクリプタを探し、使用中にして添字を返す。
static int alloc_desc()
{
    for (int i = 0; i < NUM; i++) {
        if (disk.free[i]) {
            disk.free[i] = 0;
            return i;
        }
    }
    return -1;
}

// ディスクリプタを空きにする。
static void free_desc(int i)
{
    if (i >= NUM)
        panic("free_desc 1");
    if (disk.free[i])
        panic("free_desc 2");
    disk.desc[i].addr = 0;
    disk.desc[i].len = 0;
    disk.desc[i].flags = 0;
    disk.desc[i].next = 0;
    disk.free[i] = 1;
    wakeup(&disk.free[0]);
}

// ディスクリプタチェーンを解放する。
static void free_chain(int i)
{
    while (1) {
        int flag = disk.desc[i].flags;
        int nxt = disk.desc[i].next;
        free_desc(i);
        if (flag & VRING_DESC_F_NEXT)
            i = nxt;
        else
            break;
    }
}

// 3つのディスクリプタを割り当てる（連続していなくてもよい）。
// ディスク転送は常に3つのディスクリプタを使う。
static int alloc3_desc(int *idx)
{
    for (int i = 0; i < 3; i++) {
        idx[i] = alloc_desc();
        if (idx[i] < 0) {
            for (int j = 0; j < i; j++)
                free_desc(idx[j]);
            return -1;
        }
    }
    return 0;
}

void virtio_disk_rw(struct buf *b, int write)
{
    uint64 sector = b->blockno * (BSIZE / 512);

    acquire(&disk.vdisk_lock);

    // 仕様の5.2節によると、レガシーブロック操作は3つのディスクリプタを使う。
    // type/reserved/sector用に1つ、データ用に1つ、1バイトのステータス結果用に1つである。

    // 3つのディスクリプタを割り当てる。
    int idx[3];
    while (1) {
        if (alloc3_desc(idx) == 0) {
            break;
        }
        sleep_prepare(&disk.free[0]);
        release(&disk.vdisk_lock);
        sleep();
        acquire(&disk.vdisk_lock);
    }

    // 3つのディスクリプタを設定する。
    // qemuのvirtio-blk.cがこれらを読む。

    struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

    if (write)
        buf0->type = VIRTIO_BLK_T_OUT; // ディスクへ書く
    else
        buf0->type = VIRTIO_BLK_T_IN; // ディスクを読む
    buf0->reserved = 0;
    buf0->sector = sector;

    disk.desc[idx[0]].addr = (uint64)buf0;
    disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
    disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
    disk.desc[idx[0]].next = idx[1];

    disk.desc[idx[1]].addr = (uint64)b->data;
    disk.desc[idx[1]].len = BSIZE;
    if (write)
        disk.desc[idx[1]].flags = 0; // デバイスがb->dataを読む
    else
        disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // デバイスがb->dataへ書く
    disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
    disk.desc[idx[1]].next = idx[2];

    disk.info[idx[0]].status = 0xff; // デバイスは成功時に0を書く
    disk.desc[idx[2]].addr = (uint64)&disk.info[idx[0]].status;
    disk.desc[idx[2]].len = 1;
    disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // デバイスがステータスへ書く
    disk.desc[idx[2]].next = 0;

    // virtio_disk_intr()用にstruct bufを記録する。
    b->disk = 1;
    disk.info[idx[0]].b = b;

    // ディスクリプタチェーンの先頭添字をデバイスに伝える。
    disk.avail->ring[disk.avail->idx % NUM] = idx[0];

    io_fence();

    // availリングに別のエントリがあることをデバイスに伝える。
    disk.avail->idx += 1; // % NUMではない...

    io_fence();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // 値はキュー番号

    // 要求が完了したとvirtio_disk_intr()が知らせるまで待つ。
    while (b->disk == 1) {
        sleep_prepare(b);
        release(&disk.vdisk_lock);
        sleep();
        acquire(&disk.vdisk_lock);
    }

    disk.info[idx[0]].b = 0;
    free_chain(idx[0]);

    release(&disk.vdisk_lock);
}

void virtio_disk_intr()
{
    acquire(&disk.vdisk_lock);

    // この割り込みを見たと伝えるまで、デバイスは次の割り込みを発生させない。
    // 次の行がそれを行う。
    // これはデバイスが「used」リングへ新しいエントリを書き込む処理と競合する
    // 可能性がある。その場合、新しい完了エントリをこの割り込みで処理し、
    // 次の割り込みですることがなくなるが、問題はない。
    *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

    io_fence();

    // デバイスはusedリングへエントリを追加するときdisk.used->idxを増やす。

    while (disk.used_idx != disk.used->idx) {
        io_fence();
        int id = disk.used->ring[disk.used_idx % NUM].id;

        if (disk.info[id].status != 0)
            panic("virtio_disk_intr status");

        struct buf *b = disk.info[id].b;
        b->disk = 0; // ディスクはbufの処理を終えた
        wakeup(b);

        disk.used_idx += 1;
    }

    release(&disk.vdisk_lock);
}
