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
    uint size;       // ファイルシステム全体のサイズ（ブロック数）
    uint nblocks;    // データブロック数
    uint ninodes;    // inode数。
    uint nlog;       // ログブロック数
    uint logstart;   // 先頭ログブロックのブロック番号
    uint inodestart; // 先頭inodeブロックのブロック番号
    uint bmapstart;  // 先頭空きマップブロックのブロック番号
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
    short nlink;             // ファイルシステム内でのこのinodeへのリンク数
    uint size;               // ファイルサイズ（バイト）
    uint addrs[NDIRECT + 1]; // データブロックのアドレス
};

// 1ブロックあたりのinode数。
#define IPB (BSIZE / sizeof(struct dinode))

// inode iを含むブロック
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// 1ブロックあたりのビットマップビット数
#define BPB (BSIZE * 8)

// ブロックbのビットを含む空きマップのブロック
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// ディレクトリはdirent構造体の列を含むファイルである。
#define DIRSIZ 14

// nameフィールドはDIRSIZ文字を含むことがあり、NUL文字で終わらない。
struct dirent {
    ushort inum;
    char name[DIRSIZ] __attribute__((nonstring));
};
