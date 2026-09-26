#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "../ulib.h"

#define FILES_MAX 16
#define FLAG_READ 1
#define FLAG_WRITE 2
#define FLAG_EOF 4
#define FLAG_ERROR 8
#define FLAG_CONSOLE 16
#define FLAG_LINE 32
#define FLAG_UNBUFFERED 64

struct _knoc_file
{
    int fd;
    int flags;
    int used;
    int unget;
    size_t out_length;
    size_t in_position;
    size_t in_length;
    unsigned char out[BUFSIZ];
    unsigned char in[BUFSIZ];
};

static FILE files[FILES_MAX] = {
    {FD_STDIN, FLAG_READ | FLAG_CONSOLE, 1, EOF, 0, 0, 0, {0}, {0}},
    {FD_STDOUT, FLAG_WRITE | FLAG_CONSOLE | FLAG_LINE, 1, EOF, 0, 0, 0, {0}, {0}},
    {FD_STDERR, FLAG_WRITE | FLAG_CONSOLE | FLAG_UNBUFFERED, 1, EOF, 0, 0, 0, {0}, {0}},
};

FILE *stdin = &files[0];
FILE *stdout = &files[1];
FILE *stderr = &files[2];

static int error_from(long code)
{
    switch (code)
    {
    case E_NOTFOUND:
        return ENOENT;
    case E_PERM:
        return EACCES;
    case E_EXISTS:
        return EEXIST;
    case E_ISDIR:
        return EISDIR;
    case E_NOTDIR:
        return ENOTDIR;
    case E_NOSPACE:
        return ENOSPC;
    case E_NOTEMPTY:
        return ENOTEMPTY;
    case E_FAULT:
        return EFAULT;
    default:
        return EIO;
    }
}

int fflush(FILE *stream)
{
    if (!stream)
    {
        for (int i = 0; i < FILES_MAX; i++)
        {
            if (files[i].used)
            {
                fflush(&files[i]);
            }
        }

        return 0;
    }

    if (stream->out_length > 0)
    {
        long written = write(stream->fd, stream->out, stream->out_length);

        if (written != (long)stream->out_length)
        {
            stream->flags |= FLAG_ERROR;
            stream->out_length = 0;
            return EOF;
        }

        stream->out_length = 0;
    }

    return 0;
}

void __knoc_stdio_flush_all(void)
{
    fflush(NULL);
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    int open_flags = 0;
    int append = 0;

    if (mode[0] == 'r')
    {
        flags = FLAG_READ;
        open_flags = O_READ;
    }
    else if (mode[0] == 'w')
    {
        flags = FLAG_WRITE;
        open_flags = O_WRITE | O_CREATE | O_TRUNC;
    }
    else if (mode[0] == 'a')
    {
        flags = FLAG_WRITE;
        open_flags = O_WRITE | O_CREATE;
        append = 1;
    }
    else
    {
        errno = EINVAL;
        return NULL;
    }

    if (strchr(mode, '+'))
    {
        flags |= FLAG_READ | FLAG_WRITE;
        open_flags |= O_READ | O_WRITE;
    }

    for (int i = 3; i < FILES_MAX; i++)
    {
        if (files[i].used)
        {
            continue;
        }

        int fd = open(path, open_flags);

        if (fd < 0)
        {
            errno = error_from(fd);
            return NULL;
        }

        FILE *f = &files[i];

        memset(f, 0, sizeof(*f));
        f->fd = fd;
        f->flags = flags;
        f->used = 1;
        f->unget = EOF;

        if (append)
        {
            file_stat_t info;

            if (stat(path, &info) == 0)
            {
                seek(fd, info.size);
            }
        }

        return f;
    }

    errno = ENOMEM;
    return NULL;
}

int fclose(FILE *stream)
{
    int result = fflush(stream);

    if (stream->fd > FD_STDERR)
    {
        close(stream->fd);
    }

    if (stream >= &files[3])
    {
        stream->used = 0;
    }

    return result;
}

static int read_console(FILE *stream)
{
    size_t length = 0;

    fflush(stdout);

    while (length < BUFSIZ - 1)
    {
        char c;

        if (read(FD_STDIN, &c, 1) <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            write(FD_STDOUT, "\n", 1);
            stream->in[length++] = '\n';
            break;
        }

        if (c == 0x7F || c == 0x08)
        {
            if (length > 0)
            {
                length--;
                write(FD_STDOUT, "\b \b", 3);
            }

            continue;
        }

        if (c == 0x04 && length == 0)
        {
            return 0;
        }

        if ((unsigned char)c < ' ' && c != '\t')
        {
            continue;
        }

        stream->in[length++] = (unsigned char)c;
        write(FD_STDOUT, &c, 1);
    }

    return (int)length;
}

static int fill(FILE *stream)
{
    long got;

    if (stream->flags & FLAG_CONSOLE)
    {
        got = read_console(stream);
    }
    else
    {
        fflush(stream);
        got = read(stream->fd, stream->in, BUFSIZ);
    }

    if (got <= 0)
    {
        stream->flags |= got < 0 ? FLAG_ERROR : FLAG_EOF;
        return EOF;
    }

    stream->in_position = 0;
    stream->in_length = (size_t)got;
    return 0;
}

int fgetc(FILE *stream)
{
    if (!(stream->flags & FLAG_READ))
    {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }

    if (stream->unget != EOF)
    {
        int c = stream->unget;

        stream->unget = EOF;
        return c;
    }

    if (stream->in_position >= stream->in_length && fill(stream) != 0)
    {
        return EOF;
    }

    return stream->in[stream->in_position++];
}

int getc(FILE *stream)
{
    return fgetc(stream);
}

int getchar(void)
{
    return fgetc(stdin);
}

int ungetc(int c, FILE *stream)
{
    if (c == EOF)
    {
        return EOF;
    }

    stream->unget = c;
    stream->flags &= ~FLAG_EOF;
    return c;
}

char *fgets(char *buffer, int size, FILE *stream)
{
    int n = 0;

    while (n < size - 1)
    {
        int c = fgetc(stream);

        if (c == EOF)
        {
            break;
        }

        buffer[n++] = (char)c;

        if (c == '\n')
        {
            break;
        }
    }

    if (n == 0)
    {
        return NULL;
    }

    buffer[n] = 0;
    return buffer;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    unsigned char *out = buffer;
    size_t total = size * count;
    size_t done = 0;

    while (done < total)
    {
        int c = fgetc(stream);

        if (c == EOF)
        {
            break;
        }

        out[done++] = (unsigned char)c;
    }

    return size ? done / size : 0;
}

int fputc(int c, FILE *stream)
{
    if (!(stream->flags & FLAG_WRITE))
    {
        stream->flags |= FLAG_ERROR;
        return EOF;
    }

    stream->in_position = stream->in_length = 0;
    stream->out[stream->out_length++] = (unsigned char)c;

    if (stream->out_length == BUFSIZ || (stream->flags & FLAG_UNBUFFERED) ||
        ((stream->flags & FLAG_LINE) && c == '\n'))
    {
        if (fflush(stream) != 0)
        {
            return EOF;
        }
    }

    return (unsigned char)c;
}

int putc(int c, FILE *stream)
{
    return fputc(c, stream);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
    const unsigned char *in = buffer;
    size_t total = size * count;

    for (size_t i = 0; i < total; i++)
    {
        if (fputc(in[i], stream) == EOF)
        {
            return size ? i / size : 0;
        }
    }

    return count;
}

int fputs(const char *text, FILE *stream)
{
    size_t n = strlen(text);

    return fwrite(text, 1, n, stream) == n ? 0 : EOF;
}

int puts(const char *text)
{
    return fputs(text, stdout) == 0 && fputc('\n', stdout) != EOF ? 0 : EOF;
}

long ftell(FILE *stream)
{
    long position = seek(stream->fd, SEEK_POSITION);

    if (position < 0)
    {
        return -1;
    }

    return position + (long)stream->out_length - (long)(stream->in_length - stream->in_position) -
           (stream->unget != EOF ? 1 : 0);
}

int fseek(FILE *stream, long offset, int whence)
{
    long target = offset;

    fflush(stream);

    if (whence == SEEK_CUR)
    {
        target += ftell(stream);
    }
    else if (whence == SEEK_END)
    {
        long end = seek(stream->fd, SEEK_SIZE);

        if (end < 0)
        {
            return -1;
        }

        target += end;
    }

    if (target < 0 || seek(stream->fd, (unsigned long)target) < 0)
    {
        errno = EINVAL;
        return -1;
    }

    stream->in_position = stream->in_length = 0;
    stream->unget = EOF;
    stream->flags &= ~FLAG_EOF;
    return 0;
}

void rewind(FILE *stream)
{
    fseek(stream, 0, SEEK_SET);
    stream->flags &= ~FLAG_ERROR;
}

int feof(FILE *stream)
{
    return (stream->flags & FLAG_EOF) != 0;
}

int ferror(FILE *stream)
{
    return (stream->flags & FLAG_ERROR) != 0;
}

void clearerr(FILE *stream)
{
    stream->flags &= ~(FLAG_EOF | FLAG_ERROR);
}

typedef struct sink
{
    FILE *stream;
    char *buffer;
    size_t size;
    size_t count;
} sink_t;

static void emit(sink_t *s, char c)
{
    if (s->stream)
    {
        fputc(c, s->stream);
    }
    else if (s->buffer && s->count + 1 < s->size)
    {
        s->buffer[s->count] = c;
    }

    s->count++;
}

static void emit_padding(sink_t *s, char c, int count)
{
    for (int i = 0; i < count; i++)
    {
        emit(s, c);
    }
}

static int format_unsigned(char *out, unsigned long long value, int base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char reverse[32];
    int n = 0;

    do
    {
        reverse[n++] = digits[value % (unsigned)base];
        value /= (unsigned)base;
    } while (value);

    for (int i = 0; i < n; i++)
    {
        out[i] = reverse[n - 1 - i];
    }

    return n;
}

static int format_fixed(char *out, int room, double value, int precision)
{
    int n = 0;
    double rounding = 0.5;

    for (int i = 0; i < precision; i++)
    {
        rounding /= 10;
    }

    value += rounding;

    unsigned long long whole = (unsigned long long)value;
    double fraction = value - (double)whole;

    n += format_unsigned(out, whole, 10, 0);

    if (precision > 0 && n < room)
    {
        out[n++] = '.';

        for (int i = 0; i < precision && n < room; i++)
        {
            fraction *= 10;

            int digit = (int)fraction;

            out[n++] = (char)('0' + digit);
            fraction -= digit;
        }
    }

    return n;
}

static int format_float(char *out, int room, double value, int precision, char kind)
{
    if (value != value)
    {
        memcpy(out, "nan", 3);
        return 3;
    }

    if (value > 1.7976931348623157e308)
    {
        memcpy(out, "inf", 3);
        return 3;
    }

    int exponent = 0;
    char lower_kind = (char)tolower(kind);

    if (lower_kind == 'e' || lower_kind == 'g')
    {
        double scaled = value;

        while (scaled >= 10)
        {
            scaled /= 10;
            exponent++;
        }

        while (scaled > 0 && scaled < 1)
        {
            scaled *= 10;
            exponent--;
        }

        if (lower_kind == 'g')
        {
            int digits = precision == 0 ? 1 : precision;

            if (exponent >= -4 && exponent < digits)
            {
                int n = format_fixed(out, room, value, digits - 1 - exponent);

                if (memchr(out, '.', (size_t)n))
                {
                    while (n > 0 && out[n - 1] == '0')
                    {
                        n--;
                    }

                    if (n > 0 && out[n - 1] == '.')
                    {
                        n--;
                    }
                }

                return n;
            }

            precision = digits - 1;
        }

        int n = format_fixed(out, room - 5, scaled, precision);

        if (n > 1 && out[0] == '1' && out[1] == '0')
        {
            n = format_fixed(out, room - 5, scaled / 10, precision);
            exponent++;
        }

        if (lower_kind == 'g' && memchr(out, '.', (size_t)n))
        {
            while (n > 0 && out[n - 1] == '0')
            {
                n--;
            }

            if (n > 0 && out[n - 1] == '.')
            {
                n--;
            }
        }

        out[n++] = isupper((unsigned char)kind) ? 'E' : 'e';
        out[n++] = exponent < 0 ? '-' : '+';

        int magnitude = exponent < 0 ? -exponent : exponent;

        if (magnitude < 10)
        {
            out[n++] = '0';
        }

        n += format_unsigned(out + n, (unsigned long long)magnitude, 10, 0);
        return n;
    }

    return format_fixed(out, room, value, precision);
}

static int format(sink_t *s, const char *f, va_list args)
{
    for (; *f; f++)
    {
        if (*f != '%')
        {
            emit(s, *f);
            continue;
        }

        f++;

        int left = 0;
        int zero = 0;
        int plus = 0;
        int space = 0;
        int alternate = 0;

        for (;; f++)
        {
            if (*f == '-')
            {
                left = 1;
            }
            else if (*f == '0')
            {
                zero = 1;
            }
            else if (*f == '+')
            {
                plus = 1;
            }
            else if (*f == ' ')
            {
                space = 1;
            }
            else if (*f == '#')
            {
                alternate = 1;
            }
            else
            {
                break;
            }
        }

        int width = 0;

        if (*f == '*')
        {
            width = va_arg(args, int);

            if (width < 0)
            {
                left = 1;
                width = -width;
            }

            f++;
        }
        else
        {
            while (isdigit((unsigned char)*f))
            {
                width = width * 10 + (*f++ - '0');
            }
        }

        int precision = -1;

        if (*f == '.')
        {
            f++;
            precision = 0;

            if (*f == '*')
            {
                precision = va_arg(args, int);
                f++;
            }
            else
            {
                while (isdigit((unsigned char)*f))
                {
                    precision = precision * 10 + (*f++ - '0');
                }
            }
        }

        int size = 0;

        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't')
        {
            size += *f == 'l' || *f == 'z' || *f == 'j' || *f == 't' ? 1 : 0;
            f++;
        }

        char body[400];
        int length = 0;
        const char *prefix = "";
        char kind = *f;

        if (kind == 0)
        {
            break;
        }

        if (kind == 'd' || kind == 'i')
        {
            long long value = size >= 2 ? va_arg(args, long long) : size == 1 ? va_arg(args, long) : va_arg(args, int);
            unsigned long long magnitude = value < 0 ? -(unsigned long long)value : (unsigned long long)value;

            prefix = value < 0 ? "-" : plus ? "+" : space ? " " : "";
            length = precision == 0 && magnitude == 0 ? 0 : format_unsigned(body, magnitude, 10, 0);
        }
        else if (kind == 'u' || kind == 'x' || kind == 'X' || kind == 'o')
        {
            unsigned long long value = size >= 2   ? va_arg(args, unsigned long long)
                                       : size == 1 ? va_arg(args, unsigned long)
                                                   : va_arg(args, unsigned int);
            int base = kind == 'o' ? 8 : kind == 'u' ? 10 : 16;

            if (alternate && value && base == 16)
            {
                prefix = kind == 'X' ? "0X" : "0x";
            }
            else if (alternate && base == 8)
            {
                prefix = "0";
            }

            length = precision == 0 && value == 0 ? 0 : format_unsigned(body, value, base, kind == 'X');
        }
        else if (kind == 'p')
        {
            prefix = "0x";
            length = format_unsigned(body, (unsigned long long)(unsigned long)va_arg(args, void *), 16, 0);
        }
        else if (kind == 'c')
        {
            body[0] = (char)va_arg(args, int);
            length = 1;
            precision = -1;
        }
        else if (kind == 's')
        {
            const char *text = va_arg(args, const char *);

            if (!text)
            {
                text = "(null)";
            }

            int n = (int)strlen(text);

            if (precision >= 0 && n > precision)
            {
                n = precision;
            }

            int pad = width > n ? width - n : 0;

            if (!left)
            {
                emit_padding(s, ' ', pad);
            }

            for (int i = 0; i < n; i++)
            {
                emit(s, text[i]);
            }

            if (left)
            {
                emit_padding(s, ' ', pad);
            }

            continue;
        }
        else if (kind == 'f' || kind == 'F' || kind == 'e' || kind == 'E' || kind == 'g' || kind == 'G')
        {
            double value = va_arg(args, double);

            prefix = value < 0 ? "-" : plus ? "+" : space ? " " : "";
            length = format_float(body, (int)sizeof(body) - 8, value < 0 ? -value : value,
                                  precision < 0 ? 6 : precision, kind);
            precision = -1;
        }
        else if (kind == 'n')
        {
            *va_arg(args, int *) = (int)s->count;
            continue;
        }
        else
        {
            emit(s, kind);
            continue;
        }

        int digits = precision > length ? precision - length : 0;
        int total = (int)strlen(prefix) + digits + length;
        int pad = width > total ? width - total : 0;

        if (!left && !(zero && precision < 0))
        {
            emit_padding(s, ' ', pad);
        }

        for (const char *p = prefix; *p; p++)
        {
            emit(s, *p);
        }

        if (!left && zero && precision < 0)
        {
            emit_padding(s, '0', pad);
        }

        emit_padding(s, '0', digits);

        for (int i = 0; i < length; i++)
        {
            emit(s, body[i]);
        }

        if (left)
        {
            emit_padding(s, ' ', pad);
        }
    }

    return (int)s->count;
}

int vsnprintf(char *buffer, size_t size, const char *f, va_list args)
{
    sink_t s = {NULL, buffer, size, 0};
    int count = format(&s, f, args);

    if (buffer && size > 0)
    {
        buffer[s.count < size ? s.count : size - 1] = 0;
    }

    return count;
}

int vsprintf(char *buffer, const char *f, va_list args)
{
    return vsnprintf(buffer, (size_t)-1 / 2, f, args);
}

int vfprintf(FILE *stream, const char *f, va_list args)
{
    sink_t s = {stream, NULL, 0, 0};

    return format(&s, f, args);
}

int vprintf(const char *f, va_list args)
{
    return vfprintf(stdout, f, args);
}

int printf(const char *f, ...)
{
    va_list args;

    va_start(args, f);

    int n = vfprintf(stdout, f, args);

    va_end(args);
    return n;
}

int fprintf(FILE *stream, const char *f, ...)
{
    va_list args;

    va_start(args, f);

    int n = vfprintf(stream, f, args);

    va_end(args);
    return n;
}

int sprintf(char *buffer, const char *f, ...)
{
    va_list args;

    va_start(args, f);

    int n = vsprintf(buffer, f, args);

    va_end(args);
    return n;
}

int snprintf(char *buffer, size_t size, const char *f, ...)
{
    va_list args;

    va_start(args, f);

    int n = vsnprintf(buffer, size, f, args);

    va_end(args);
    return n;
}

int sscanf(const char *text, const char *f, ...)
{
    va_list args;
    int assigned = 0;
    const char *p = text;

    va_start(args, f);

    for (; *f; f++)
    {
        if (isspace((unsigned char)*f))
        {
            while (isspace((unsigned char)*p))
            {
                p++;
            }

            continue;
        }

        if (*f != '%')
        {
            if (*p != *f)
            {
                break;
            }

            p++;
            continue;
        }

        f++;

        int width = 0;
        int size = 0;

        while (isdigit((unsigned char)*f))
        {
            width = width * 10 + (*f++ - '0');
        }

        while (*f == 'l')
        {
            size++;
            f++;
        }

        if (*f != 'c')
        {
            while (isspace((unsigned char)*p))
            {
                p++;
            }
        }

        char *end = (char *)p;

        if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x')
        {
            long long value = strtoll(p, &end, *f == 'x' ? 16 : *f == 'i' ? 0 : 10);

            if (end == p)
            {
                break;
            }

            if (size >= 2)
            {
                *va_arg(args, long long *) = value;
            }
            else if (size == 1)
            {
                *va_arg(args, long *) = (long)value;
            }
            else
            {
                *va_arg(args, int *) = (int)value;
            }
        }
        else if (*f == 'f' || *f == 'g' || *f == 'e')
        {
            double value = strtod(p, &end);

            if (end == p)
            {
                break;
            }

            if (size >= 1)
            {
                *va_arg(args, double *) = value;
            }
            else
            {
                *va_arg(args, float *) = (float)value;
            }
        }
        else if (*f == 's')
        {
            char *out = va_arg(args, char *);
            int taken = 0;

            while (*end && !isspace((unsigned char)*end) && (width == 0 || taken < width))
            {
                *out++ = *end++;
                taken++;
            }

            *out = 0;

            if (end == p)
            {
                break;
            }
        }
        else if (*f == 'c')
        {
            if (!*p)
            {
                break;
            }

            *va_arg(args, char *) = *end++;
        }
        else
        {
            break;
        }

        p = end;
        assigned++;
    }

    va_end(args);
    return assigned;
}

void perror(const char *text)
{
    if (text && *text)
    {
        fputs(text, stderr);
        fputs(": ", stderr);
    }

    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}
