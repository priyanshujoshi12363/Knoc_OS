#ifndef _KNOC_STDIO_H
#define _KNOC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FILENAME_MAX 128

typedef struct _knoc_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
char *fgets(char *buffer, int size, FILE *stream);
int ungetc(int c, FILE *stream);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);
int putchar(int c);
int fputs(const char *text, FILE *stream);
int puts(const char *text);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int remove(const char *path);
int rename(const char *from, const char *to);

int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *buffer, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vprintf(const char *format, va_list args);
int vfprintf(FILE *stream, const char *format, va_list args);
int vsprintf(char *buffer, const char *format, va_list args);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int sscanf(const char *text, const char *format, ...);
void perror(const char *text);

#endif
