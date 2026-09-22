#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat // ホストのstruct statとの衝突を避ける
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b)                                                    \
    do {                                                                       \
        switch (0)                                                             \
        case 0:                                                                \
        case (a):;                                                             \
    } while (0)
#endif

#define NINODES 200

// ディスク配置:
// [ ブートブロック | sbブロック | ログ | inodeブロック | 空きビットマップ | データブロック ]

int bitmap_block_count = FSSIZE / BPB + 1;
int inode_block_count = NINODES / IPB + 1;
int log_block_count = LOGBLOCKS + 1;
int metadata_block_count;
int data_block_count;

int file_system_fd;
struct superblock file_system_superblock;
char empty_block[BSIZE];
uint next_free_inode_number = 1;
uint next_free_block_number;

void write_allocation_bitmap(int allocated_block_count);
void write_sector(uint sector_number, void *buffer);
void write_inode(uint inode_number, struct dinode *disk_inode);
void read_inode(uint inode_number, struct dinode *disk_inode_out);
void read_sector(uint sector_number, void *buffer);
uint allocate_inode(ushort type);
void append_inode_data(uint inode_number, void *source, int byte_count);
void die(const char *message);

// RISC-Vのバイト順に変換する
ushort encode_uint16(ushort value)
{
    ushort encoded_value;
    uchar *bytes = (uchar *)&encoded_value;
    bytes[0] = value;
    bytes[1] = value >> 8;
    return encoded_value;
}

uint encode_uint32(uint value)
{
    uint encoded_value;
    uchar *bytes = (uchar *)&encoded_value;
    bytes[0] = value;
    bytes[1] = value >> 8;
    bytes[2] = value >> 16;
    bytes[3] = value >> 24;
    return encoded_value;
}

int main(int argc, char *argv[])
{
    int i, bytes_read, fd;
    uint root_inode_number, inode_number, offset;
    struct dirent directory_entry;
    char block_buffer[BSIZE];
    struct dinode disk_inode;

    static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

    if (argc < 2) {
        fprintf(stderr, "Usage: mkfs fs.img files...\n");
        exit(1);
    }

    assert((BSIZE % sizeof(struct dinode)) == 0);
    assert((BSIZE % sizeof(struct dirent)) == 0);

    file_system_fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (file_system_fd < 0)
        die(argv[1]);

    // FSの1ブロック = ディスクの1セクタ
    metadata_block_count =
        2 + log_block_count + inode_block_count + bitmap_block_count;
    data_block_count = FSSIZE - metadata_block_count;

    file_system_superblock.magic = FSMAGIC;
    file_system_superblock.total_block_count = encode_uint32(FSSIZE);
    file_system_superblock.data_block_count = encode_uint32(data_block_count);
    file_system_superblock.inode_count = encode_uint32(NINODES);
    file_system_superblock.log_block_count = encode_uint32(log_block_count);
    file_system_superblock.log_start_block = encode_uint32(2);
    file_system_superblock.inode_start_block =
        encode_uint32(2 + log_block_count);
    file_system_superblock.bitmap_start_block =
        encode_uint32(2 + log_block_count + inode_block_count);

    printf(
        "nmeta %d (boot, super, log blocks %u, inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
        metadata_block_count, log_block_count, inode_block_count,
        bitmap_block_count, data_block_count, FSSIZE);

    next_free_block_number = metadata_block_count;

    for (i = 0; i < FSSIZE; i++)
        write_sector(i, empty_block);

    memset(block_buffer, 0, sizeof(block_buffer));
    memmove(block_buffer, &file_system_superblock,
            sizeof(file_system_superblock));
    write_sector(1, block_buffer);

    root_inode_number = allocate_inode(T_DIR);
    assert(root_inode_number == ROOTINO);

    bzero(&directory_entry, sizeof(directory_entry));
    directory_entry.inode_number = encode_uint16(root_inode_number);
    strcpy(directory_entry.name, ".");
    append_inode_data(root_inode_number, &directory_entry,
                      sizeof(directory_entry));

    bzero(&directory_entry, sizeof(directory_entry));
    directory_entry.inode_number = encode_uint16(root_inode_number);
    strcpy(directory_entry.name, "..");
    append_inode_data(root_inode_number, &directory_entry,
                      sizeof(directory_entry));

    for (i = 2; i < argc; i++) {
        // "user/"を取り除く
        char *shortname;
        if (strncmp(argv[i], "user/", 5) == 0)
            shortname = argv[i] + 5;
        else
            shortname = argv[i];

        assert(index(shortname, '/') == 0);

        if ((fd = open(argv[i], 0)) < 0)
            die(argv[i]);

        // ファイルシステムへ書き込むとき、名前の先頭の_を飛ばす。
        // バイナリを_rm、_catなどと名付けることで、ビルド用OSが
        // rmやcatのようなシステムバイナリの代わりにそれらを実行するのを防ぐ。
        if (shortname[0] == '_')
            shortname += 1;

        assert(strlen(shortname) <= DIRSIZ);

        inode_number = allocate_inode(T_FILE);

        bzero(&directory_entry, sizeof(directory_entry));
        directory_entry.inode_number = encode_uint16(inode_number);
        strncpy(directory_entry.name, shortname, DIRSIZ);
        append_inode_data(root_inode_number, &directory_entry,
                          sizeof(directory_entry));

        while ((bytes_read = read(fd, block_buffer,
                                  sizeof(block_buffer))) > 0)
            append_inode_data(inode_number, block_buffer, bytes_read);

        close(fd);
    }

    // ルートinodeディレクトリのサイズを修正する
    read_inode(root_inode_number, &disk_inode);
    offset = encode_uint32(disk_inode.size);
    offset = ((offset / BSIZE) + 1) * BSIZE;
    disk_inode.size = encode_uint32(offset);
    write_inode(root_inode_number, &disk_inode);

    write_allocation_bitmap(next_free_block_number);

    exit(0);
}

void write_sector(uint sector_number, void *buffer)
{
    if (lseek(file_system_fd, sector_number * BSIZE, 0) !=
        sector_number * BSIZE)
        die("lseek");
    if (write(file_system_fd, buffer, BSIZE) != BSIZE)
        die("write");
}

void write_inode(uint inode_number, struct dinode *disk_inode)
{
    char block_buffer[BSIZE];
    uint block_number;
    struct dinode *target_inode;

    block_number = IBLOCK(inode_number, file_system_superblock);
    read_sector(block_number, block_buffer);
    target_inode = ((struct dinode *)block_buffer) + (inode_number % IPB);
    *target_inode = *disk_inode;
    write_sector(block_number, block_buffer);
}

void read_inode(uint inode_number, struct dinode *disk_inode_out)
{
    char block_buffer[BSIZE];
    uint block_number;
    struct dinode *source_inode;

    block_number = IBLOCK(inode_number, file_system_superblock);
    read_sector(block_number, block_buffer);
    source_inode = ((struct dinode *)block_buffer) + (inode_number % IPB);
    *disk_inode_out = *source_inode;
}

void read_sector(uint sector_number, void *buffer)
{
    if (lseek(file_system_fd, sector_number * BSIZE, 0) !=
        sector_number * BSIZE)
        die("lseek");
    if (read(file_system_fd, buffer, BSIZE) != BSIZE)
        die("read");
}

uint allocate_inode(ushort type)
{
    uint inode_number = next_free_inode_number++;
    struct dinode disk_inode;

    bzero(&disk_inode, sizeof(disk_inode));
    disk_inode.type = encode_uint16(type);
    disk_inode.link_count = encode_uint16(1);
    disk_inode.size = encode_uint32(0);
    write_inode(inode_number, &disk_inode);
    return inode_number;
}

void write_allocation_bitmap(int allocated_block_count)
{
    uchar bitmap[BSIZE];
    int i;

    printf("balloc: first %d blocks have been allocated\n",
           allocated_block_count);
    assert(allocated_block_count < BPB);
    bzero(bitmap, BSIZE);
    for (i = 0; i < allocated_block_count; i++) {
        bitmap[i / 8] = bitmap[i / 8] | (0x1 << (i % 8));
    }
    printf("balloc: write bitmap block at sector %d\n",
           file_system_superblock.bitmap_start_block);
    write_sector(file_system_superblock.bitmap_start_block, bitmap);
}

#define MIN(left, right) ((left) < (right) ? (left) : (right))

void append_inode_data(uint inode_number, void *source, int byte_count)
{
    char *source_bytes = (char *)source;
    uint file_block_number, offset, bytes_to_copy;
    struct dinode disk_inode;
    char block_buffer[BSIZE];
    uint indirect_blocks[NINDIRECT];
    uint disk_block_number;

    read_inode(inode_number, &disk_inode);
    offset = encode_uint32(disk_inode.size);
    while (byte_count > 0) {
        file_block_number = offset / BSIZE;
        assert(file_block_number < MAXFILE);
        if (file_block_number < NDIRECT) {
            if (encode_uint32(disk_inode.block_addresses[file_block_number]) ==
                0) {
                disk_inode.block_addresses[file_block_number] =
                    encode_uint32(next_free_block_number++);
            }
            disk_block_number = encode_uint32(
                disk_inode.block_addresses[file_block_number]);
        } else {
            if (encode_uint32(disk_inode.block_addresses[NDIRECT]) == 0) {
                disk_inode.block_addresses[NDIRECT] =
                    encode_uint32(next_free_block_number++);
            }
            read_sector(encode_uint32(disk_inode.block_addresses[NDIRECT]),
                        (char *)indirect_blocks);
            if (indirect_blocks[file_block_number - NDIRECT] == 0) {
                indirect_blocks[file_block_number - NDIRECT] =
                    encode_uint32(next_free_block_number++);
                write_sector(
                    encode_uint32(disk_inode.block_addresses[NDIRECT]),
                    (char *)indirect_blocks);
            }
            disk_block_number =
                encode_uint32(indirect_blocks[file_block_number - NDIRECT]);
        }
        bytes_to_copy = MIN(byte_count,
                            (file_block_number + 1) * BSIZE - offset);
        read_sector(disk_block_number, block_buffer);
        bcopy(source_bytes,
              block_buffer + offset - (file_block_number * BSIZE),
              bytes_to_copy);
        write_sector(disk_block_number, block_buffer);
        byte_count -= bytes_to_copy;
        offset += bytes_to_copy;
        source_bytes += bytes_to_copy;
    }
    disk_inode.size = encode_uint32(offset);
    write_inode(inode_number, &disk_inode);
}

void die(const char *message)
{
    perror(message);
    exit(1);
}
