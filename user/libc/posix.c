#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <inttypes.h>
#include "../../kernel/syscall_abi.h"

#define POSIX_WRONLY 1
#define POSIX_RDWR 2
#define POSIX_ACCMODE 3
#define POSIX_CREAT 0100
#define POSIX_EXCL 0200
#define POSIX_TRUNC 01000
#define POSIX_APPEND 02000

long syscall(long number, long a0, long a1, long a2);
unsigned long uptime(void);
long realtime(void);
long random_bytes(void *buffer, unsigned long length);

static int fail(long code)
{
    switch (code)
    {
    case E_NOTFOUND:
        errno = ENOENT;
        break;
    case E_PERM:
        errno = EACCES;
        break;
    case E_EXISTS:
        errno = EEXIST;
        break;
    case E_ISDIR:
        errno = EISDIR;
        break;
    case E_NOTDIR:
        errno = ENOTDIR;
        break;
    case E_NOSPACE:
        errno = ENOSPC;
        break;
    case E_NOTEMPTY:
        errno = ENOTEMPTY;
        break;
    case E_BADF:
        errno = EBADF;
        break;
    case E_INVAL:
        errno = EINVAL;
        break;
    case E_FAULT:
        errno = EFAULT;
        break;
    default:
        errno = EIO;
        break;
    }

    return -1;
}

int __knoc_open(const char *path, int flags)
{
    return (int)syscall(SYS_OPEN, (long)path, flags, 0);
}

long __knoc_size_of(const char *path)
{
    file_stat_t info;
    long result = syscall(SYS_STAT, (long)path, (long)&info, 0);

    return result == 0 ? (long)info.size : result;
}

int open(const char *path, int flags, ...)
{
    int mode = flags & POSIX_ACCMODE;
    int knoc = mode == POSIX_WRONLY ? O_WRITE : mode == POSIX_RDWR ? O_READ | O_WRITE : O_READ;

    if (flags & POSIX_CREAT)
    {
        knoc |= O_CREATE;

        if ((flags & POSIX_EXCL) && __knoc_size_of(path) >= 0)
        {
            errno = EEXIST;
            return -1;
        }
    }

    if (flags & POSIX_TRUNC)
    {
        knoc |= O_TRUNC;
    }

    long fd = syscall(SYS_OPEN, (long)path, knoc, 0);

    if (fd < 0)
    {
        return fail(fd);
    }

    if (flags & POSIX_APPEND)
    {
        long size = syscall(SYS_SEEK, fd, (long)SEEK_SIZE, 0);

        if (size >= 0)
        {
            syscall(SYS_SEEK, fd, size, 0);
        }
    }

    return (int)fd;
}

int creat(const char *path, mode_t mode)
{
    (void)mode;
    return open(path, POSIX_CREAT | POSIX_WRONLY | POSIX_TRUNC);
}

off_t lseek(int fd, off_t offset, int whence)
{
    long base = 0;

    if (whence == SEEK_CUR)
    {
        base = syscall(SYS_SEEK, fd, (long)SEEK_POSITION, 0);
    }
    else if (whence == SEEK_END)
    {
        base = syscall(SYS_SEEK, fd, (long)SEEK_SIZE, 0);
    }

    if (base < 0)
    {
        return fail(base);
    }

    if (base + offset < 0)
    {
        errno = EINVAL;
        return -1;
    }

    long result = syscall(SYS_SEEK, fd, base + offset, 0);

    return result < 0 ? fail(result) : result;
}

int unlink(const char *path)
{
    long result = syscall(SYS_REMOVE, (long)path, 0, 0);

    return result < 0 ? fail(result) : 0;
}

int rmdir(const char *path)
{
    return unlink(path);
}

int stat(const char *path, struct stat *out)
{
    file_stat_t info;
    long result = syscall(SYS_STAT, (long)path, (long)&info, 0);

    if (result < 0)
    {
        return fail(result);
    }

    memset(out, 0, sizeof(*out));
    out->st_mode = info.type == FILE_TYPE_DIR ? S_IFDIR | 0755 : S_IFREG | 0644;
    out->st_size = (off_t)info.size;
    out->st_nlink = 1;
    out->st_blksize = 4096;
    out->st_blocks = (blkcnt_t)((info.size + 511) / 512);
    return 0;
}

int fstat(int fd, struct stat *out)
{
    long size = syscall(SYS_SEEK, fd, (long)SEEK_SIZE, 0);

    memset(out, 0, sizeof(*out));

    if (fd <= 2)
    {
        out->st_mode = 0020000 | 0666;
        return 0;
    }

    if (size < 0)
    {
        return fail(size);
    }

    out->st_mode = S_IFREG | 0644;
    out->st_size = size;
    out->st_nlink = 1;
    out->st_blksize = 4096;
    return 0;
}

int chmod(const char *path, mode_t mode)
{
    struct stat info;

    (void)mode;
    return stat(path, &info);
}

int access(const char *path, int mode)
{
    struct stat info;

    (void)mode;
    return stat(path, &info);
}

char *getcwd(char *buffer, size_t size)
{
    if (!buffer)
    {
        size = size ? size : PATH_MAX;
        buffer = malloc(size);

        if (!buffer)
        {
            return NULL;
        }
    }

    long result = syscall(SYS_GETCWD, (long)buffer, (long)size, 0);

    if (result < 0)
    {
        fail(result == E_INVAL ? E_INVAL : result);
        errno = result == E_INVAL ? ERANGE : errno;
        return NULL;
    }

    return buffer;
}

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}

void _exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0);

    while (1)
    {
    }
}

int execvp(const char *file, char *const argv[])
{
    (void)file;
    (void)argv;
    errno = ENOENT;
    return -1;
}

int system(const char *command)
{
    (void)command;
    return -1;
}

static void normalize(const char *joined, char *out)
{
    size_t length = 0;
    size_t i = 0;

    while (joined[i])
    {
        while (joined[i] == '/')
        {
            i++;
        }

        size_t start = i;

        while (joined[i] && joined[i] != '/')
        {
            i++;
        }

        size_t part = i - start;

        if (part == 0 || (part == 1 && joined[start] == '.'))
        {
            continue;
        }

        if (part == 2 && joined[start] == '.' && joined[start + 1] == '.')
        {
            while (length > 0 && out[length - 1] != '/')
            {
                length--;
            }

            if (length > 0)
            {
                length--;
            }

            continue;
        }

        if (length + part + 2 >= PATH_MAX)
        {
            break;
        }

        out[length++] = '/';
        memcpy(out + length, joined + start, part);
        length += part;
    }

    if (length == 0)
    {
        out[length++] = '/';
    }

    out[length] = 0;
}

char *realpath(const char *path, char *resolved)
{
    char joined[PATH_MAX * 2];
    struct stat info;

    if (stat(path, &info) != 0)
    {
        return NULL;
    }

    if (path[0] == '/')
    {
        strncpy(joined, path, sizeof(joined) - 1);
        joined[sizeof(joined) - 1] = 0;
    }
    else
    {
        if (!getcwd(joined, PATH_MAX))
        {
            return NULL;
        }

        strcat(joined, "/");
        strncat(joined, path, sizeof(joined) - strlen(joined) - 1);
    }

    if (!resolved)
    {
        resolved = malloc(PATH_MAX);

        if (!resolved)
        {
            return NULL;
        }
    }

    normalize(joined, resolved);
    return resolved;
}

int gettimeofday(struct timeval *value, void *zone)
{
    unsigned long ticks = uptime();
    long seconds = realtime();

    (void)zone;
    value->tv_sec = seconds > 0 ? (time_t)seconds : (time_t)(ticks / 100);
    value->tv_usec = (suseconds_t)((ticks % 100) * 10000);
    return 0;
}

int getentropy(void *buffer, size_t length)
{
    if (length > RANDOM_MAX)
    {
        errno = EIO;
        return -1;
    }

    if (random_bytes(buffer, length) != (long)length)
    {
        errno = ENOSYS;
        return -1;
    }

    return 0;
}

intmax_t strtoimax(const char *text, char **end, int base)
{
    return strtoll(text, end, base);
}

uintmax_t strtoumax(const char *text, char **end, int base)
{
    return strtoull(text, end, base);
}
