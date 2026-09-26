#ifndef _KNOC_UNISTD_H
#define _KNOC_UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

ssize_t read(int fd, void *buffer, size_t length);
ssize_t write(int fd, const void *buffer, size_t length);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
char *getcwd(char *buffer, size_t size);
int chdir(const char *path);
int isatty(int fd);
int getpid(void);
int execvp(const char *file, char *const argv[]);
int getentropy(void *buffer, size_t length);
void _exit(int code) __attribute__((noreturn));

#endif
