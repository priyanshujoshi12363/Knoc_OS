#include "knocfs.h"
#include "process.h"
#include "string.h"
#include "syscall_abi.h"

#define SECTOR_SIZE 512
#define SECTORS_PER_BLOCK (KNOCFS_BLOCK_SIZE / SECTOR_SIZE)
#define INODES_PER_BLOCK (KNOCFS_BLOCK_SIZE / sizeof(knocfs_inode_t))
#define DIRENTS_PER_BLOCK (KNOCFS_BLOCK_SIZE / sizeof(knocfs_dirent_t))
#define BITS_PER_BLOCK (KNOCFS_BLOCK_SIZE * 8)
#define BLACKBOX_RESERVED_SECTORS 8
#define NO_BLOCK 0xFFFFFFFFU

_Static_assert(sizeof(knocfs_inode_t) == 128, "inodes are 128 bytes");
_Static_assert(sizeof(knocfs_dirent_t) == 64, "directory entries are 64 bytes");

static device_t *fs_disk;
static knocfs_super_t super;
static int mounted;

/* One operation at a time: operations sleep during disk I/O */
static sleeplock_t fs_lock = SLEEPLOCK_INIT;

static uint8_t block_buffer[KNOCFS_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t inode_buffer[KNOCFS_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t bitmap_buffer[KNOCFS_BLOCK_SIZE] __attribute__((aligned(16)));
static uint8_t zero_block[KNOCFS_BLOCK_SIZE];
static uint32_t bitmap_cached = NO_BLOCK;
static int bitmap_dirty;

/* ---- Blocks ---- */

static int read_block(uint32_t block, void *buffer)
{
    return device_read_blocks(fs_disk,
                              super.start_sector + (uint64_t)block * SECTORS_PER_BLOCK,
                              SECTORS_PER_BLOCK,
                              buffer);
}

static int write_block(uint32_t block, const void *buffer)
{
    return device_write_blocks(fs_disk,
                               super.start_sector + (uint64_t)block * SECTORS_PER_BLOCK,
                               SECTORS_PER_BLOCK,
                               buffer);
}

/* ---- Free-block bitmap (one bitmap block cached) ---- */

static int bitmap_flush(void)
{
    if (bitmap_dirty && bitmap_cached != NO_BLOCK)
    {
        if (write_block(super.bitmap_start + bitmap_cached, bitmap_buffer) != 0)
        {
            return -1;
        }
    }

    bitmap_dirty = 0;
    return 0;
}

static int bitmap_load(uint32_t block)
{
    uint32_t index = block / BITS_PER_BLOCK;

    if (index == bitmap_cached)
    {
        return 0;
    }

    if (bitmap_flush() != 0 || read_block(super.bitmap_start + index, bitmap_buffer) != 0)
    {
        bitmap_cached = NO_BLOCK;
        return -1;
    }

    bitmap_cached = index;
    return 0;
}

static int block_used(uint32_t block)
{
    if (block >= super.total_blocks || bitmap_load(block) != 0)
    {
        return 1;
    }

    uint32_t bit = block % BITS_PER_BLOCK;

    return (bitmap_buffer[bit / 8] >> (bit % 8)) & 1;
}

static void block_mark(uint32_t block, int used)
{
    if (bitmap_load(block) != 0)
    {
        return;
    }

    uint32_t bit = block % BITS_PER_BLOCK;

    if (used)
    {
        bitmap_buffer[bit / 8] |= (uint8_t)(1 << (bit % 8));
    }
    else
    {
        bitmap_buffer[bit / 8] &= (uint8_t)~(1 << (bit % 8));
    }

    bitmap_dirty = 1;
}

/* First run of `want` free blocks, or the longest run if none is that long */
static int alloc_run(uint32_t want, knocfs_extent_t *run)
{
    uint32_t best_start = 0;
    uint32_t best_count = 0;
    uint32_t start = 0;
    uint32_t count = 0;

    for (uint32_t block = super.data_start; block < super.total_blocks; block++)
    {
        if (block_used(block))
        {
            count = 0;
            continue;
        }

        if (count == 0)
        {
            start = block;
        }

        count++;

        if (count > best_count)
        {
            best_start = start;
            best_count = count;
        }

        if (count == want)
        {
            break;
        }
    }

    if (best_count == 0)
    {
        return -1;
    }

    for (uint32_t i = 0; i < best_count; i++)
    {
        block_mark(best_start + i, 1);
    }

    run->start = best_start;
    run->count = best_count;
    return 0;
}

static void free_extents(knocfs_inode_t *inode)
{
    for (uint32_t i = 0; i < inode->extent_count; i++)
    {
        for (uint32_t b = 0; b < inode->extents[i].count; b++)
        {
            block_mark(inode->extents[i].start + b, 0);
        }
    }

    inode->extent_count = 0;
    inode->size = 0;
}

/* ---- Inodes ---- */

static int inode_get(uint32_t number, knocfs_inode_t *inode)
{
    if (number == 0 || number >= super.inode_count ||
        read_block(super.inode_start + number / INODES_PER_BLOCK, inode_buffer) != 0)
    {
        return -1;
    }

    memcpy(inode, inode_buffer + (number % INODES_PER_BLOCK) * sizeof(*inode), sizeof(*inode));
    return 0;
}

static int inode_put(uint32_t number, const knocfs_inode_t *inode)
{
    uint32_t block = super.inode_start + number / INODES_PER_BLOCK;

    if (read_block(block, inode_buffer) != 0)
    {
        return -1;
    }

    memcpy(inode_buffer + (number % INODES_PER_BLOCK) * sizeof(*inode), inode, sizeof(*inode));
    return write_block(block, inode_buffer);
}

static int inode_alloc(uint32_t *number)
{
    for (uint32_t block = 0; block < super.inode_blocks; block++)
    {
        if (read_block(super.inode_start + block, inode_buffer) != 0)
        {
            return -1;
        }

        for (uint32_t i = 0; i < INODES_PER_BLOCK; i++)
        {
            uint32_t candidate = block * INODES_PER_BLOCK + i;
            knocfs_inode_t *inode = (knocfs_inode_t *)(inode_buffer + i * sizeof(knocfs_inode_t));

            if (candidate != 0 && candidate < super.inode_count && inode->type == KNOCFS_TYPE_FREE)
            {
                *number = candidate;
                return 0;
            }
        }
    }

    return -1;
}

static uint32_t allocated_blocks(const knocfs_inode_t *inode)
{
    uint32_t total = 0;

    for (uint32_t i = 0; i < inode->extent_count; i++)
    {
        total += inode->extents[i].count;
    }

    return total;
}

/* Disk block holding block `index` of the file */
static uint32_t file_block(const knocfs_inode_t *inode, uint32_t index)
{
    for (uint32_t i = 0; i < inode->extent_count; i++)
    {
        if (index < inode->extents[i].count)
        {
            return inode->extents[i].start + index;
        }

        index -= inode->extents[i].count;
    }

    return NO_BLOCK;
}

/* Give the file `needed` blocks: grow the last extent in place if the
   next blocks are free, otherwise add new (as long as possible) extents */
static int grow(knocfs_inode_t *inode, uint32_t needed)
{
    uint32_t have = allocated_blocks(inode);

    while (have < needed)
    {
        uint32_t want = needed - have;

        if (inode->extent_count > 0)
        {
            knocfs_extent_t *last = &inode->extents[inode->extent_count - 1];
            uint32_t next = last->start + last->count;

            if (next < super.total_blocks && !block_used(next))
            {
                block_mark(next, 1);
                last->count++;
                have++;
                continue;
            }
        }

        if (inode->extent_count >= KNOCFS_EXTENTS)
        {
            return E_NOSPACE;
        }

        knocfs_extent_t run;

        if (alloc_run(want, &run) != 0)
        {
            return E_NOSPACE;
        }

        inode->extents[inode->extent_count++] = run;
        have += run.count;
    }

    return 0;
}

/* ---- File data (callers hold fs_lock) ---- */

static int64_t read_locked(uint32_t number, uint64_t offset, void *buffer, uint64_t length)
{
    knocfs_inode_t inode;

    if (inode_get(number, &inode) != 0 || inode.type == KNOCFS_TYPE_FREE)
    {
        return E_IO;
    }

    if (offset >= inode.size)
    {
        return 0;
    }

    if (length > inode.size - offset)
    {
        length = inode.size - offset;
    }

    uint8_t *to = (uint8_t *)buffer;
    uint64_t done = 0;

    while (done < length)
    {
        uint64_t position = offset + done;
        uint32_t block = file_block(&inode, (uint32_t)(position / KNOCFS_BLOCK_SIZE));
        uint64_t within = position % KNOCFS_BLOCK_SIZE;
        uint64_t chunk = KNOCFS_BLOCK_SIZE - within;

        if (chunk > length - done)
        {
            chunk = length - done;
        }

        if (block == NO_BLOCK)
        {
            return E_IO;
        }

        if (within == 0 && chunk == KNOCFS_BLOCK_SIZE)
        {
            if (read_block(block, to + done) != 0)
            {
                return E_IO;
            }
        }
        else
        {
            if (read_block(block, block_buffer) != 0)
            {
                return E_IO;
            }

            memcpy(to + done, block_buffer + within, chunk);
        }

        done += chunk;
    }

    return (int64_t)done;
}

static int64_t write_locked(uint32_t number, uint64_t offset, const void *buffer, uint64_t length)
{
    knocfs_inode_t inode;

    if (inode_get(number, &inode) != 0 || inode.type == KNOCFS_TYPE_FREE)
    {
        return E_IO;
    }

    if (length == 0)
    {
        return 0;
    }

    uint64_t end = offset + length;
    uint32_t old_blocks = allocated_blocks(&inode);
    uint32_t needed = (uint32_t)((end + KNOCFS_BLOCK_SIZE - 1) / KNOCFS_BLOCK_SIZE);
    int result = grow(&inode, needed);

    if (result != 0)
    {
        bitmap_cached = NO_BLOCK;
        bitmap_dirty = 0;
        return result;
    }

    /* New blocks the write doesn't reach (a gap after a seek) must not
       show old disk contents */
    for (uint32_t index = old_blocks; index < offset / KNOCFS_BLOCK_SIZE; index++)
    {
        if (write_block(file_block(&inode, index), zero_block) != 0)
        {
            return E_IO;
        }
    }

    const uint8_t *from = (const uint8_t *)buffer;
    uint64_t done = 0;

    while (done < length)
    {
        uint64_t position = offset + done;
        uint32_t index = (uint32_t)(position / KNOCFS_BLOCK_SIZE);
        uint32_t block = file_block(&inode, index);
        uint64_t within = position % KNOCFS_BLOCK_SIZE;
        uint64_t chunk = KNOCFS_BLOCK_SIZE - within;

        if (chunk > length - done)
        {
            chunk = length - done;
        }

        if (within == 0 && chunk == KNOCFS_BLOCK_SIZE)
        {
            if (write_block(block, from + done) != 0)
            {
                return E_IO;
            }
        }
        else
        {
            if (index >= old_blocks)
            {
                memset(block_buffer, 0, KNOCFS_BLOCK_SIZE);
            }
            else if (read_block(block, block_buffer) != 0)
            {
                return E_IO;
            }

            memcpy(block_buffer + within, from + done, chunk);

            if (write_block(block, block_buffer) != 0)
            {
                return E_IO;
            }
        }

        done += chunk;
    }

    if (end > inode.size)
    {
        inode.size = end;
    }

    if (bitmap_flush() != 0 || inode_put(number, &inode) != 0)
    {
        return E_IO;
    }

    return (int64_t)done;
}

/* ---- Directories ---- */

static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

/* Finds `name` in a directory; `slot` gets its entry index, or the first
   free index if it's missing */
static int dir_find(uint32_t directory, const char *name, uint32_t *inode, uint32_t *slot)
{
    knocfs_inode_t dir;
    knocfs_dirent_t entry;
    uint32_t free_slot = NO_BLOCK;

    if (inode_get(directory, &dir) != 0 || dir.type != KNOCFS_TYPE_DIR)
    {
        return E_NOTDIR;
    }

    uint32_t count = (uint32_t)(dir.size / sizeof(entry));

    for (uint32_t i = 0; i < count; i++)
    {
        if (read_locked(directory, (uint64_t)i * sizeof(entry), &entry, sizeof(entry)) != sizeof(entry))
        {
            return E_IO;
        }

        if (entry.inode == 0)
        {
            if (free_slot == NO_BLOCK)
            {
                free_slot = i;
            }

            continue;
        }

        if (names_equal(entry.name, name))
        {
            *inode = entry.inode;

            if (slot != 0)
            {
                *slot = i;
            }

            return 0;
        }
    }

    if (slot != 0)
    {
        *slot = free_slot != NO_BLOCK ? free_slot : count;
    }

    return E_NOTFOUND;
}

/* Walks the path; with `parent` set, stops at the last component and
   returns its parent directory and name instead */
static int resolve(const char *path, uint32_t *inode, uint32_t *parent, char *last)
{
    char name[KNOCFS_NAME_MAX];
    uint32_t current = KNOCFS_ROOT_INODE;

    if (path == 0 || path[0] != '/')
    {
        return E_INVAL;
    }

    while (*path == '/')
    {
        path++;
    }

    if (*path == 0)
    {
        if (parent != 0)
        {
            return E_INVAL;
        }

        *inode = KNOCFS_ROOT_INODE;
        return 0;
    }

    while (1)
    {
        uint32_t length = 0;

        while (path[length] && path[length] != '/')
        {
            if (length >= KNOCFS_NAME_MAX - 1)
            {
                return E_INVAL;
            }

            name[length] = path[length];
            length++;
        }

        name[length] = 0;
        path += length;

        while (*path == '/')
        {
            path++;
        }

        if (*path == 0 && parent != 0)
        {
            *parent = current;
            memcpy(last, name, length + 1);
            return 0;
        }

        uint32_t next;
        int result = dir_find(current, name, &next, 0);

        if (result != 0)
        {
            return result;
        }

        current = next;

        if (*path == 0)
        {
            *inode = current;
            return 0;
        }
    }
}

/* ---- Public API ---- */

int knocfs_mount(device_t *disk)
{
    uint8_t sector[SECTOR_SIZE];

    mounted = 0;

    if (disk == 0 || !disk->ready ||
        disk->block_count <= KNOCFS_START_SECTOR + BLACKBOX_RESERVED_SECTORS ||
        device_read_block(disk, KNOCFS_START_SECTOR, sector) != 0)
    {
        return -1;
    }

    memcpy(&super, sector, sizeof(super));

    if (super.magic != KNOCFS_MAGIC ||
        super.version != KNOCFS_VERSION ||
        super.block_size != KNOCFS_BLOCK_SIZE ||
        super.start_sector != KNOCFS_START_SECTOR ||
        super.start_sector + (uint64_t)super.total_blocks * SECTORS_PER_BLOCK >
            disk->block_count - BLACKBOX_RESERVED_SECTORS ||
        super.data_start >= super.total_blocks)
    {
        return -1;
    }

    fs_disk = disk;
    bitmap_cached = NO_BLOCK;
    bitmap_dirty = 0;

    knocfs_inode_t root;

    if (inode_get(KNOCFS_ROOT_INODE, &root) != 0 || root.type != KNOCFS_TYPE_DIR)
    {
        fs_disk = 0;
        return -1;
    }

    mounted = 1;
    return 0;
}

int knocfs_mounted(void)
{
    return mounted;
}

void knocfs_usage(uint64_t *total_bytes, uint64_t *free_bytes, uint32_t *files)
{
    *total_bytes = 0;
    *free_bytes = 0;
    *files = 0;

    if (!mounted)
    {
        return;
    }

    sleeplock_acquire(&fs_lock);

    uint64_t free_count = 0;

    for (uint32_t block = super.data_start; block < super.total_blocks; block++)
    {
        free_count += !block_used(block);
    }

    for (uint32_t number = 1; number < super.inode_count; number++)
    {
        knocfs_inode_t inode;

        if (number % INODES_PER_BLOCK == 0 || number == 1)
        {
            if (read_block(super.inode_start + number / INODES_PER_BLOCK, inode_buffer) != 0)
            {
                break;
            }
        }

        memcpy(&inode, inode_buffer + (number % INODES_PER_BLOCK) * sizeof(inode), sizeof(inode));
        *files += inode.type == KNOCFS_TYPE_FILE;
    }

    *total_bytes = (uint64_t)(super.total_blocks - super.data_start) * KNOCFS_BLOCK_SIZE;
    *free_bytes = free_count * KNOCFS_BLOCK_SIZE;

    sleeplock_release(&fs_lock);
}

int knocfs_lookup(const char *path, uint32_t *inode)
{
    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);
    int result = resolve(path, inode, 0, 0);
    sleeplock_release(&fs_lock);

    return result;
}

int knocfs_create(const char *path, uint16_t type, uint32_t *number)
{
    char name[KNOCFS_NAME_MAX];
    uint32_t parent;
    uint32_t existing;
    uint32_t slot;

    if (!mounted || (type != KNOCFS_TYPE_FILE && type != KNOCFS_TYPE_DIR))
    {
        return E_INVAL;
    }

    sleeplock_acquire(&fs_lock);

    int result = resolve(path, 0, &parent, name);

    if (result == 0 && name[0] == 0)
    {
        result = E_INVAL;
    }

    if (result == 0)
    {
        result = dir_find(parent, name, &existing, &slot);
        result = result == 0 ? E_EXISTS : (result == E_NOTFOUND ? 0 : result);
    }

    if (result == 0 && inode_alloc(number) != 0)
    {
        result = E_NOSPACE;
    }

    if (result == 0)
    {
        knocfs_inode_t inode;
        knocfs_dirent_t entry;

        memset(&inode, 0, sizeof(inode));
        inode.type = type;

        memset(&entry, 0, sizeof(entry));
        entry.inode = *number;
        memcpy(entry.name, name, sizeof(entry.name));

        if (inode_put(*number, &inode) != 0 ||
            write_locked(parent, (uint64_t)slot * sizeof(entry), &entry, sizeof(entry)) != sizeof(entry))
        {
            result = E_IO;
        }
    }

    sleeplock_release(&fs_lock);
    return result;
}

int knocfs_remove(const char *path)
{
    char name[KNOCFS_NAME_MAX];
    uint32_t parent;
    uint32_t number;
    uint32_t slot;
    knocfs_inode_t inode;

    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);

    int result = resolve(path, 0, &parent, name);

    if (result == 0)
    {
        result = dir_find(parent, name, &number, &slot);
    }

    if (result == 0 && inode_get(number, &inode) != 0)
    {
        result = E_IO;
    }

    if (result == 0 && inode.type == KNOCFS_TYPE_DIR)
    {
        knocfs_dirent_t entry;

        for (uint64_t offset = 0; offset < inode.size; offset += sizeof(entry))
        {
            if (read_locked(number, offset, &entry, sizeof(entry)) == sizeof(entry) && entry.inode != 0)
            {
                result = E_NOTEMPTY;
                break;
            }
        }
    }

    if (result == 0)
    {
        knocfs_dirent_t empty;

        memset(&empty, 0, sizeof(empty));
        free_extents(&inode);
        inode.type = KNOCFS_TYPE_FREE;

        if (bitmap_flush() != 0 ||
            inode_put(number, &inode) != 0 ||
            write_locked(parent, (uint64_t)slot * sizeof(empty), &empty, sizeof(empty)) != sizeof(empty))
        {
            result = E_IO;
        }
    }

    sleeplock_release(&fs_lock);
    return result;
}

static int inside(const char *path, const char *folder)
{
    int i = 0;

    while (folder[i] && path[i] == folder[i])
    {
        i++;
    }

    return folder[i] == 0 && (path[i] == '/' || path[i] == 0);
}

int knocfs_rename(const char *from, const char *to)
{
    char old_name[KNOCFS_NAME_MAX];
    char new_name[KNOCFS_NAME_MAX];
    uint32_t old_parent;
    uint32_t new_parent;
    uint32_t number;
    uint32_t existing;
    uint32_t old_slot;
    uint32_t new_slot;

    if (!mounted)
    {
        return E_IO;
    }

    if (inside(to, from))
    {
        return E_INVAL;
    }

    sleeplock_acquire(&fs_lock);

    int result = resolve(from, 0, &old_parent, old_name);

    if (result == 0)
    {
        result = resolve(to, 0, &new_parent, new_name);
    }

    if (result == 0 && (old_name[0] == 0 || new_name[0] == 0))
    {
        result = E_INVAL;
    }

    if (result == 0)
    {
        result = dir_find(old_parent, old_name, &number, &old_slot);
    }

    if (result == 0)
    {
        result = dir_find(new_parent, new_name, &existing, &new_slot);
        result = result == 0 ? E_EXISTS : (result == E_NOTFOUND ? 0 : result);
    }

    if (result == 0)
    {
        knocfs_dirent_t entry;
        knocfs_dirent_t empty;

        memset(&entry, 0, sizeof(entry));
        entry.inode = number;
        memcpy(entry.name, new_name, sizeof(entry.name));
        memset(&empty, 0, sizeof(empty));

        if (write_locked(new_parent, (uint64_t)new_slot * sizeof(entry), &entry, sizeof(entry)) != sizeof(entry))
        {
            result = E_IO;
        }
        else if (new_parent == old_parent && new_slot == old_slot)
        {
            result = 0;
        }
        else if (write_locked(old_parent, (uint64_t)old_slot * sizeof(empty), &empty, sizeof(empty)) != sizeof(empty))
        {
            result = E_IO;
        }
    }

    sleeplock_release(&fs_lock);
    return result;
}

int knocfs_stat(uint32_t number, knocfs_stat_t *stat)
{
    knocfs_inode_t inode;

    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);
    int result = inode_get(number, &inode) == 0 && inode.type != KNOCFS_TYPE_FREE ? 0 : E_NOTFOUND;
    sleeplock_release(&fs_lock);

    if (result == 0)
    {
        stat->type = inode.type;
        stat->extents = inode.extent_count;
        stat->size = inode.size;
    }

    return result;
}

int knocfs_readdir(uint32_t directory, uint32_t index, knocfs_dirent_t *entry)
{
    knocfs_inode_t dir;
    int result = E_NOTFOUND;

    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);

    if (inode_get(directory, &dir) != 0 || dir.type != KNOCFS_TYPE_DIR)
    {
        result = E_NOTDIR;
    }
    else
    {
        uint32_t seen = 0;

        for (uint64_t offset = 0; offset < dir.size; offset += sizeof(*entry))
        {
            if (read_locked(directory, offset, entry, sizeof(*entry)) != sizeof(*entry))
            {
                result = E_IO;
                break;
            }

            if (entry->inode != 0 && seen++ == index)
            {
                result = 0;
                break;
            }
        }
    }

    sleeplock_release(&fs_lock);
    return result;
}

int knocfs_truncate(uint32_t number)
{
    knocfs_inode_t inode;

    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);

    int result = inode_get(number, &inode) == 0 && inode.type == KNOCFS_TYPE_FILE ? 0 : E_INVAL;

    if (result == 0)
    {
        free_extents(&inode);
        result = bitmap_flush() == 0 && inode_put(number, &inode) == 0 ? 0 : E_IO;
    }

    sleeplock_release(&fs_lock);
    return result;
}

int64_t knocfs_read(uint32_t number, uint64_t offset, void *buffer, uint64_t length)
{
    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);
    int64_t result = read_locked(number, offset, buffer, length);
    sleeplock_release(&fs_lock);

    return result;
}

int64_t knocfs_write(uint32_t number, uint64_t offset, const void *buffer, uint64_t length)
{
    knocfs_inode_t inode;

    if (!mounted)
    {
        return E_IO;
    }

    sleeplock_acquire(&fs_lock);

    int64_t result = inode_get(number, &inode) == 0 && inode.type == KNOCFS_TYPE_FILE
                         ? write_locked(number, offset, buffer, length)
                         : E_ISDIR;

    sleeplock_release(&fs_lock);
    return result;
}
