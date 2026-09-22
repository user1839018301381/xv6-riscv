struct file {
    enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
    int reference_count;
    char is_readable;
    char is_writable;
    struct pipe *pipe; // FD_PIPEの場合
    struct inode *inode;
    uint offset;
    short major;       // FD_DEVICEの場合のメジャー番号
};

#define major(device_number) ((device_number) >> 16 & 0xFFFF)
#define minor(device_number) ((device_number) & 0xFFFF)
#define mkdev(major_number, minor_number)                                     \
    ((uint)((major_number) << 16 | (minor_number)))

// inodeのメモリ上のコピー
struct inode {
    uint device;
    uint inode_number;
    int reference_count;
    struct sleeplock lock; // ここから下の全てを保護する
    int is_valid;

    short type; // ディスク上のinodeのコピー
    short major;
    short minor;
    short link_count;
    uint size;
    uint block_addresses[NDIRECT + 1];
};

// メジャーデバイス番号からデバイス操作関数への対応表。
struct devsw {
    int (*read)(int source_is_user, uint64 destination_address,
                int byte_count);
    int (*write)(int destination_is_user, uint64 source_address,
                 int byte_count);
};

extern struct devsw devsw[];

#define CONSOLE 1
