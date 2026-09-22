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

// virtio MMIOレジスタのアドレス。
#define R(register_offset) ((volatile uint32 *)(VIRTIO0 + (register_offset)))

static struct disk {
    // DMAディスクリプタの集合（リングではない）。
    // ドライバはこれで個々のディスク操作の読み書き場所をデバイスに伝える。
    // ディスクリプタはNUM個ある。
    // ほとんどのコマンドは、これらのディスクリプタ数個による
    // 「チェーン」（連結リスト）で構成される。
    struct virtq_desc *descriptors;

    // ドライバがデバイスに処理してほしいディスクリプタ番号を書くリング。
    // 各チェーンの先頭ディスクリプタだけを含み、NUM個の要素がある。
    struct virtq_avail *available_ring;

    // デバイスが処理を終えたディスクリプタ番号を書くリング
    // （各チェーンの先頭だけ）。usedリングエントリはNUM個ある。
    struct virtq_used *used_ring;

    // ドライバ自身の管理情報。
    char descriptor_is_free[NUM];
    uint16 next_used_index;

    // 実行中の操作の情報を追跡する。
    // 完了割り込みが到着したときに使う。
    // チェーン先頭のディスクリプタ添字で索引する。
    struct {
        struct buf *buffer;
        char status;
    } requests[NUM];

    // ディスクコマンドヘッダ。
    // 便利なようにディスクリプタと1対1で対応させる。
    struct virtio_blk_req operation_headers[NUM];

    struct spinlock lock;

} disk_state;

void virtio_disk_init(void)
{
    uint32 status = 0;

    initlock(&disk_state.lock, "virtio_disk");

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
    uint32 max_queue_size = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max_queue_size == 0)
        panic("virtio disk has no queue 0");
    if (max_queue_size < NUM)
        panic("virtio disk max queue too short");

    // キューメモリを割り当ててゼロクリアする。
    disk_state.descriptors = kalloc();
    disk_state.available_ring = kalloc();
    disk_state.used_ring = kalloc();
    if (!disk_state.descriptors || !disk_state.available_ring ||
        !disk_state.used_ring)
        panic("virtio disk kalloc");
    memset(disk_state.descriptors, 0, PGSIZE);
    memset(disk_state.available_ring, 0, PGSIZE);
    memset(disk_state.used_ring, 0, PGSIZE);

    // キューサイズを設定する。
    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    // 物理アドレスを書き込む。
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk_state.descriptors;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk_state.descriptors >> 32;
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk_state.available_ring;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk_state.available_ring >> 32;
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk_state.used_ring;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk_state.used_ring >> 32;

    // キューの準備完了。
    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    // NUM個のディスクリプタはすべて未使用で開始する。
    for (int i = 0; i < NUM; i++)
        disk_state.descriptor_is_free[i] = 1;

    // 完全に準備完了したことをデバイスに伝える。
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // plic.cとtrap.cがVIRTIO0_IRQからの割り込みを設定する。
}

// 空きディスクリプタを探し、使用中にして添字を返す。
static int allocate_descriptor(void)
{
    for (int i = 0; i < NUM; i++) {
        if (disk_state.descriptor_is_free[i]) {
            disk_state.descriptor_is_free[i] = 0;
            return i;
        }
    }
    return -1;
}

// ディスクリプタを空きにする。
static void release_descriptor(int descriptor_index)
{
    if (descriptor_index >= NUM)
        panic("release_descriptor 1");
    if (disk_state.descriptor_is_free[descriptor_index])
        panic("release_descriptor 2");
    disk_state.descriptors[descriptor_index].addr = 0;
    disk_state.descriptors[descriptor_index].len = 0;
    disk_state.descriptors[descriptor_index].flags = 0;
    disk_state.descriptors[descriptor_index].next = 0;
    disk_state.descriptor_is_free[descriptor_index] = 1;
    wakeup(&disk_state.descriptor_is_free[0]);
}

// ディスクリプタチェーンを解放する。
static void release_descriptor_chain(int descriptor_index)
{
    while (1) {
        int descriptor_flags =
            disk_state.descriptors[descriptor_index].flags;
        int next_descriptor_index =
            disk_state.descriptors[descriptor_index].next;
        release_descriptor(descriptor_index);
        if (descriptor_flags & VRING_DESC_F_NEXT)
            descriptor_index = next_descriptor_index;
        else
            break;
    }
}

// 3つのディスクリプタを割り当てる（連続していなくてもよい）。
// ディスク転送は常に3つのディスクリプタを使う。
static int allocate_three_descriptors(int *descriptor_indices)
{
    for (int i = 0; i < 3; i++) {
        descriptor_indices[i] = allocate_descriptor();
        if (descriptor_indices[i] < 0) {
            for (int j = 0; j < i; j++)
                release_descriptor(descriptor_indices[j]);
            return -1;
        }
    }
    return 0;
}

void virtio_disk_rw(struct buf *buffer, int is_write)
{
    uint64 sector_number = buffer->block_number * (BSIZE / 512);

    acquire(&disk_state.lock);

    // 仕様の5.2節によると、レガシーブロック操作は3つのディスクリプタを使う。
    // type/reserved/sector用に1つ、データ用に1つ、1バイトのステータス結果用に1つである。

    // 3つのディスクリプタを割り当てる。
    int descriptor_indices[3];
    while (1) {
        if (allocate_three_descriptors(descriptor_indices) == 0) {
            break;
        }
        sleep_prepare(&disk_state.descriptor_is_free[0]);
        release(&disk_state.lock);
        sleep();
        acquire(&disk_state.lock);
    }

    // 3つのディスクリプタを設定する。
    // qemuのvirtio-blk.cがこれらを読む。

    struct virtio_blk_req *request_header =
        &disk_state.operation_headers[descriptor_indices[0]];

    if (is_write)
        request_header->type = VIRTIO_BLK_T_OUT; // ディスクへ書く
    else
        request_header->type = VIRTIO_BLK_T_IN; // ディスクを読む
    request_header->reserved = 0;
    request_header->sector = sector_number;

    disk_state.descriptors[descriptor_indices[0]].addr =
        (uint64)request_header;
    disk_state.descriptors[descriptor_indices[0]].len =
        sizeof(struct virtio_blk_req);
    disk_state.descriptors[descriptor_indices[0]].flags = VRING_DESC_F_NEXT;
    disk_state.descriptors[descriptor_indices[0]].next = descriptor_indices[1];

    disk_state.descriptors[descriptor_indices[1]].addr =
        (uint64)buffer->block_data;
    disk_state.descriptors[descriptor_indices[1]].len = BSIZE;
    if (is_write)
        disk_state.descriptors[descriptor_indices[1]].flags = 0;
    else
        disk_state.descriptors[descriptor_indices[1]].flags =
            VRING_DESC_F_WRITE;
    disk_state.descriptors[descriptor_indices[1]].flags |=
        VRING_DESC_F_NEXT;
    disk_state.descriptors[descriptor_indices[1]].next = descriptor_indices[2];

    disk_state.requests[descriptor_indices[0]].status = 0xff;
    disk_state.descriptors[descriptor_indices[2]].addr =
        (uint64)&disk_state.requests[descriptor_indices[0]].status;
    disk_state.descriptors[descriptor_indices[2]].len = 1;
    disk_state.descriptors[descriptor_indices[2]].flags = VRING_DESC_F_WRITE;
    disk_state.descriptors[descriptor_indices[2]].next = 0;

    // virtio_disk_intr()用にstruct bufを記録する。
    buffer->is_disk_owned = 1;
    disk_state.requests[descriptor_indices[0]].buffer = buffer;

    // ディスクリプタチェーンの先頭添字をデバイスに伝える。
    disk_state.available_ring->ring[disk_state.available_ring->idx % NUM] =
        descriptor_indices[0];

    io_fence();

    // availリングに別のエントリがあることをデバイスに伝える。
    disk_state.available_ring->idx += 1; // % NUMではない...

    io_fence();

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // 値はキュー番号

    // 要求が完了したとvirtio_disk_intr()が知らせるまで待つ。
    while (buffer->is_disk_owned == 1) {
        sleep_prepare(buffer);
        release(&disk_state.lock);
        sleep();
        acquire(&disk_state.lock);
    }

    disk_state.requests[descriptor_indices[0]].buffer = 0;
    release_descriptor_chain(descriptor_indices[0]);

    release(&disk_state.lock);
}

void virtio_disk_intr()
{
    acquire(&disk_state.lock);

    // この割り込みを見たと伝えるまで、デバイスは次の割り込みを発生させない。
    // 次の行がそれを行う。
    // これはデバイスが「used」リングへ新しいエントリを書き込む処理と競合する
    // 可能性がある。その場合、新しい完了エントリをこの割り込みで処理し、
    // 次の割り込みですることがなくなるが、問題はない。
    *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

    io_fence();

    // デバイスはusedリングへエントリを追加するときdisk_state.used_ring->idxを増やす。

    while (disk_state.next_used_index != disk_state.used_ring->idx) {
        io_fence();
        int descriptor_index =
            disk_state.used_ring->ring[
                disk_state.next_used_index % NUM].id;

        if (disk_state.requests[descriptor_index].status != 0)
            panic("virtio_disk_intr status");

        struct buf *buffer = disk_state.requests[descriptor_index].buffer;
        buffer->is_disk_owned = 0;
        wakeup(buffer);

        disk_state.next_used_index += 1;
    }

    release(&disk_state.lock);
}
