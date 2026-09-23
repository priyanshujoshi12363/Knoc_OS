#ifndef KNOCFS_H
#define KNOCFS_H

#include <stdint.h>
#include "device.h"

/* KnocFS on-disk format. tools/knocfs.py (the host tool) writes the same
   layout, so keep the two in sync.

   disk sectors: [0..2047] boot test data | KnocFS | [last 8] AI black box

   KnocFS blocks (4 KiB each, numbered from the start of the filesystem):
   0              superblock
   1..            free-block bitmap (1 bit per block)
   ..             inode table (128-byte inodes, inode 1 = root directory)
   ..end          data blocks

   A file is a list of extents (start block, block count), so a large
   file written in one piece is one contiguous run on the disk. */

#define KNOCFS_MAGIC 0x31305346434F4E4BULL
#define KNOCFS_VERSION 1
#define KNOCFS_BLOCK_SIZE 4096
#define KNOCFS_START_SECTOR 2048
#define KNOCFS_EXTENTS 12
#define KNOCFS_NAME_MAX 60
#define KNOCFS_ROOT_INODE 1

#define KNOCFS_TYPE_FREE 0
#define KNOCFS_TYPE_FILE 1
#define KNOCFS_TYPE_DIR 2

typedef struct knocfs_super
{
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t start_sector;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t data_start;
    uint32_t reserved;
} knocfs_super_t;

typedef struct knocfs_extent
{
    uint32_t start;
    uint32_t count;
} knocfs_extent_t;

typedef struct knocfs_inode
{
    uint16_t type;
    uint16_t reserved;
    uint32_t extent_count;
    uint64_t size;
    knocfs_extent_t extents[KNOCFS_EXTENTS];
    uint64_t padding[2];
} knocfs_inode_t;

typedef struct knocfs_dirent
{
    uint32_t inode;
    char name[KNOCFS_NAME_MAX];
} knocfs_dirent_t;

typedef struct knocfs_stat
{
    uint32_t type;
    uint32_t extents;
    uint64_t size;
} knocfs_stat_t;

int knocfs_mount(device_t *disk);
int knocfs_mounted(void);
void knocfs_usage(uint64_t *total_bytes, uint64_t *free_bytes, uint32_t *files);

int knocfs_lookup(const char *path, uint32_t *inode);
int knocfs_create(const char *path, uint16_t type, uint32_t *inode);
int knocfs_remove(const char *path);
int knocfs_rename(const char *from, const char *to);
int knocfs_stat(uint32_t inode, knocfs_stat_t *stat);
int knocfs_readdir(uint32_t directory, uint32_t index, knocfs_dirent_t *entry);
int knocfs_truncate(uint32_t inode);

int64_t knocfs_read(uint32_t inode, uint64_t offset, void *buffer, uint64_t length);
int64_t knocfs_write(uint32_t inode, uint64_t offset, const void *buffer, uint64_t length);

#endif
