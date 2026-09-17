struct file {
    enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
    int ref; // 参照カウント
    char readable;
    char writable;
    struct pipe *pipe; // FD_PIPEの場合
    struct inode *ip;  // FD_INODEおよびFD_DEVICEの場合
    uint off;          // FD_INODEの場合のオフセット
    short major;       // FD_DEVICEの場合のメジャー番号
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define mkdev(m, n) ((uint)((m) << 16 | (n)))

// inodeのメモリ上のコピー
struct inode {
    uint dev;              // デバイス番号
    uint inum;             // inode番号
    int ref;               // 参照カウント
    struct sleeplock lock; // ここから下の全てを保護する
    int valid;             // inodeはディスクから読み込まれたか?

    short type; // ディスク上のinodeのコピー
    short major;
    short minor;
    short nlink;
    uint size;
    uint addrs[NDIRECT + 1];
};

// メジャーデバイス番号からデバイス操作関数への対応表。
struct devsw {
    int (*read)(int, uint64, int);
    int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
