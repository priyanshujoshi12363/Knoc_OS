#include <stdlib.h>
#include <string.h>
#include <errno.h>

void *memmove(void *destination, const void *source, size_t length)
{
    unsigned char *to = destination;
    const unsigned char *from = source;

    if (to == from || length == 0)
    {
        return destination;
    }

    if (to < from || to >= from + length)
    {
        for (size_t i = 0; i < length; i++)
        {
            to[i] = from[i];
        }
    }
    else
    {
        for (size_t i = length; i > 0; i--)
        {
            to[i - 1] = from[i - 1];
        }
    }

    return destination;
}

int memcmp(const void *a, const void *b, size_t length)
{
    const unsigned char *x = a;
    const unsigned char *y = b;

    for (size_t i = 0; i < length; i++)
    {
        if (x[i] != y[i])
        {
            return x[i] < y[i] ? -1 : 1;
        }
    }

    return 0;
}

void *memchr(const void *memory, int c, size_t length)
{
    const unsigned char *p = memory;

    for (size_t i = 0; i < length; i++)
    {
        if (p[i] == (unsigned char)c)
        {
            return (void *)(p + i);
        }
    }

    return NULL;
}

size_t strnlen(const char *text, size_t max)
{
    size_t n = 0;

    while (n < max && text[n])
    {
        n++;
    }

    return n;
}

char *strncpy(char *destination, const char *source, size_t length)
{
    size_t i = 0;

    for (; i < length && source[i]; i++)
    {
        destination[i] = source[i];
    }

    for (; i < length; i++)
    {
        destination[i] = 0;
    }

    return destination;
}

char *strcat(char *destination, const char *source)
{
    strcpy(destination + strlen(destination), source);
    return destination;
}

char *strncat(char *destination, const char *source, size_t length)
{
    char *end = destination + strlen(destination);
    size_t i = 0;

    for (; i < length && source[i]; i++)
    {
        end[i] = source[i];
    }

    end[i] = 0;
    return destination;
}

int strncmp(const char *a, const char *b, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (a[i] != b[i] || a[i] == 0)
        {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }

    return 0;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
    {
        a++;
        b++;
    }

    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        int x = lower((unsigned char)a[i]);
        int y = lower((unsigned char)b[i]);

        if (x != y || x == 0)
        {
            return x - y;
        }
    }

    return 0;
}

char *strchr(const char *text, int c)
{
    for (;; text++)
    {
        if (*text == (char)c)
        {
            return (char *)text;
        }

        if (*text == 0)
        {
            return NULL;
        }
    }
}

char *strrchr(const char *text, int c)
{
    const char *found = NULL;

    for (;; text++)
    {
        if (*text == (char)c)
        {
            found = text;
        }

        if (*text == 0)
        {
            return (char *)found;
        }
    }
}

char *strstr(const char *text, const char *part)
{
    size_t n = strlen(part);

    if (n == 0)
    {
        return (char *)text;
    }

    for (; *text; text++)
    {
        if (strncmp(text, part, n) == 0)
        {
            return (char *)text;
        }
    }

    return NULL;
}

char *strcasestr(const char *text, const char *part)
{
    size_t n = strlen(part);

    for (; *text; text++)
    {
        if (strncasecmp(text, part, n) == 0)
        {
            return (char *)text;
        }
    }

    return n == 0 ? (char *)text : NULL;
}

size_t strspn(const char *text, const char *accept)
{
    size_t n = 0;

    while (text[n] && strchr(accept, text[n]))
    {
        n++;
    }

    return n;
}

size_t strcspn(const char *text, const char *reject)
{
    size_t n = 0;

    while (text[n] && !strchr(reject, text[n]))
    {
        n++;
    }

    return n;
}

char *strpbrk(const char *text, const char *accept)
{
    text += strcspn(text, accept);
    return *text ? (char *)text : NULL;
}

char *strtok(char *text, const char *delimiters)
{
    static char *next;

    if (text == NULL)
    {
        text = next;
    }

    if (text == NULL)
    {
        return NULL;
    }

    text += strspn(text, delimiters);

    if (*text == 0)
    {
        next = NULL;
        return NULL;
    }

    char *end = text + strcspn(text, delimiters);

    if (*end)
    {
        *end = 0;
        next = end + 1;
    }
    else
    {
        next = NULL;
    }

    return text;
}

char *strdup(const char *text)
{
    return strndup(text, strlen(text));
}

char *strndup(const char *text, size_t length)
{
    size_t n = strnlen(text, length);
    char *copy = malloc(n + 1);

    if (copy)
    {
        memcpy(copy, text, n);
        copy[n] = 0;
    }

    return copy;
}

char *strerror(int code)
{
    switch (code)
    {
    case 0:
        return "no error";
    case EPERM:
    case EACCES:
        return "permission denied";
    case ENOENT:
        return "no such file or folder";
    case EIO:
        return "disk error";
    case EBADF:
        return "bad file";
    case ENOMEM:
        return "out of memory";
    case EFAULT:
        return "bad address";
    case EEXIST:
        return "already exists";
    case ENOTDIR:
        return "not a folder";
    case EISDIR:
        return "is a folder";
    case EINVAL:
        return "invalid argument";
    case ENOSPC:
        return "no space left";
    case ERANGE:
        return "result out of range";
    case ENOTEMPTY:
        return "folder not empty";
    case ENOSYS:
        return "not supported on this machine";
    case ENETDOWN:
        return "network is down";
    case ETIMEDOUT:
        return "timed out";
    case ECONNREFUSED:
        return "connection refused";
    default:
        return "unknown error";
    }
}
