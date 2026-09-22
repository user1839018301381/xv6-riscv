// ディスク上のファイルシステム形式。
// カーネルとユーザプログラムの両方がこのヘッダファイルを使う。

#define ROOTINO 1    // ルートのi番号
#define BSIZE   1024 // ブロックサイズ

// ディスク配置:
// [ブートブロック | スーパーブロック | ログ | inodeブロック |
//                                          空きビットマップ | データブロック]
//
// mkfsはスーパーブロックを計算し初期ファイルシステムを構築する。
// スーパーブロックはディスク配置を記述する:
struct superblock {
    uint magic;      // FSMAGICでなければならない
    uint total_block_count;
    uint data_block_count;
    uint inode_count;
    uint log_block_count;
    uint log_start_block;
    uint inode_start_block;
    uint bitmap_start_block;
};

#define FSMAGIC 0x10203040

#define NDIRECT   12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE   (NDIRECT + NINDIRECT)
#define NLINK_MAX 32767 // nlinkはshort型なので上限を超えるリンクを拒否する

// ディスク上のinode構造
struct dinode {
    short type;              // ファイル種別
    short major;             // メジャーデバイス番号（T_DEVICEのみ）
    short minor;             // マイナーデバイス番号（T_DEVICEのみ）
    short link_count;
    uint size;               // ファイルサイズ（バイト）
    uint block_addresses[NDIRECT + 1];
};

// 1ブロックあたりのinode数。
#define IPB (BSIZE / sizeof(struct dinode))

// inode iを含むブロック
#define IBLOCK(inode_number, superblock)                                      \
    ((inode_number) / IPB + (superblock).inode_start_block)

// 1ブロックあたりのビットマップビット数
#define BPB (BSIZE * 8)

// ブロックbのビットを含む空きマップのブロック
#define BBLOCK(block_number, superblock)                                      \
    ((block_number) / BPB + (superblock).bitmap_start_block)

// ディレクトリはdirent構造体の列を含むファイルである。
#define DIRSIZ 14

// nameフィールドはDIRSIZ文字を含むことがあり、NUL文字で終わらない。
struct dirent {
    ushort inode_number;
    char name[DIRSIZ] __attribute__((nonstring));
};
