struct buf {
    int valid; // ディスクからデータが読み込まれたか?
    int disk;  // ディスクがこのbufを「所有」しているか?
    uint dev;
    uint blockno;
    struct sleeplock lock;
    uint refcnt;
    struct buf *prev; // LRUキャッシュリスト
    struct buf *next;
    uchar data[BSIZE];
};
