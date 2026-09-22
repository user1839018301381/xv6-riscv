#define T_DIR    1 // ディレクトリ
#define T_FILE   2 // ファイル
#define T_DEVICE 3 // デバイス

struct stat {
    int device;  // ファイルシステムのディスクデバイス
    uint inode_number;
    short type;  // ファイル種別
    short link_count;
    uint64 size; // ファイルサイズ（バイト）
};
