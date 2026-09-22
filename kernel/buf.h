struct buf {
    int is_valid;
    int is_disk_owned;
    uint device;
    uint block_number;
    struct sleeplock lock;
    uint reference_count;
    struct buf *previous; // LRUキャッシュリスト
    struct buf *next;
    uchar block_data[BSIZE];
};
