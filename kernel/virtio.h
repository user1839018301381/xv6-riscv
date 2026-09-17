//
// virtioデバイス定義。
// MMIOインタフェースとvirtioディスクリプタの両方用。
// qemuでのみテスト済み。
//
// virtio仕様:
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
//

// 0x10001000からマップされるvirtio MMIO制御レジスタ。
// qemuのvirtio_mmio.h由来。
// clang-format off
#define VIRTIO_MMIO_MAGIC_VALUE		0x000 // 0x74726976
#define VIRTIO_MMIO_VERSION		0x004 // バージョン。2でなければならない
#define VIRTIO_MMIO_DEVICE_ID		0x008 // デバイス種別。1はネットワーク、2はディスク
#define VIRTIO_MMIO_VENDOR_ID		0x00c // 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_QUEUE_SEL		0x030 // キューを選択する。書き込み専用
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034 // 現在のキューの最大サイズ。読み取り専用
#define VIRTIO_MMIO_QUEUE_NUM		0x038 // 現在のキューのサイズ。書き込み専用
#define VIRTIO_MMIO_QUEUE_READY		0x044 // 準備完了ビット
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050 // 書き込み専用
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060 // 読み取り専用
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064 // 書き込み専用
#define VIRTIO_MMIO_STATUS		0x070 // 読み書き
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080 // ディスクリプタ表の物理アドレス。書き込み専用
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW	0x090 // availリングの物理アドレス。書き込み専用
#define VIRTIO_MMIO_DRIVER_DESC_HIGH	0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW	0x0a0 // usedリングの物理アドレス。書き込み専用
#define VIRTIO_MMIO_DEVICE_DESC_HIGH	0x0a4
// clang-format on

// qemuのvirtio_config.h由来のステータスレジスタビット
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER      2
#define VIRTIO_CONFIG_S_DRIVER_OK   4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

// デバイス機能ビット
#define VIRTIO_BLK_F_RO             5  /* ディスクは読み取り専用 */
#define VIRTIO_BLK_F_SCSI           7  /* SCSIコマンドのパススルーに対応 */
#define VIRTIO_BLK_F_FLUSH          9  /* キャッシュフラッシュコマンドに対応 */
#define VIRTIO_BLK_F_CONFIG_WCE     11 /* 設定でライトバックモードを利用可能 */
#define VIRTIO_BLK_F_MQ             12 /* 複数のvqに対応 */
#define VIRTIO_F_ANY_LAYOUT         27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX     29

// virtioディスクリプタの数。
// 2のべき乗でなければならない。
#define NUM 8

// 仕様で定義された単一のディスクリプタ。
struct virtq_desc {
    uint64 addr;
    uint32 len;
    uint16 flags;
    uint16 next;
};
#define VRING_DESC_F_NEXT  1 // 別のディスクリプタと連結
#define VRING_DESC_F_WRITE 2 // デバイスが書き込む（読み取りの反対）

// 仕様で定義された（全体の）availリング。
struct virtq_avail {
    uint16 flags;     // 常に0
    uint16 idx;       // ドライバが次にring[idx]へ書き込む
    uint16 ring[NUM]; // チェーン先頭のディスクリプタ番号
    uint16 unused;
};

// デバイスが完了した要求をドライバに知らせる「used」リングの1エントリ。
struct virtq_used_elem {
    uint32 id; // 完了したディスクリプタチェーン先頭の添字
    uint32 len;
};

struct virtq_used {
    uint16 flags; // 常に0
    uint16 idx;   // デバイスはring[]エントリ追加時に増やす
    struct virtq_used_elem ring[NUM];
};

// 仕様の5.2節で説明されるvirtioブロックデバイス（ディスクなど）固有の定義。

#define VIRTIO_BLK_T_IN  0 // ディスクを読む
#define VIRTIO_BLK_T_OUT 1 // ディスクへ書く

// ディスク要求の最初のディスクリプタの形式。
// ブロックと1バイトのステータスを含む、さらに2つのディスクリプタが続く。
struct virtio_blk_req {
    uint32 type; // VIRTIO_BLK_T_INまたは..._OUT
    uint32 reserved;
    uint64 sector;
};
