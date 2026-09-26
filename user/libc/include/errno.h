#ifndef _KNOC_ERRNO_H
#define _KNOC_ERRNO_H

extern int errno;

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39
#define ENETDOWN 100
#define ETIMEDOUT 110
#define ECONNREFUSED 111

#endif
