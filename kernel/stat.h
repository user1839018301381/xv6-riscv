#define T_DIR    1 // ディレクトリ
#define T_FILE   2 // ファイル
#define T_DEVICE 3 // デバイス

struct stat {
    int dev;     // ファイルシステムのディスクデバイス
    uint ino;    // inode番号
    short type;  // ファイル種別
    short nlink; // ファイルへのリンク数
    uint64 size; // ファイルサイズ（バイト）
};
