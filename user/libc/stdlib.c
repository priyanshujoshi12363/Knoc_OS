#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include "../ulib.h"

#define HEADER 16
#define CHUNK (256UL * 1024)
#define ATEXIT_MAX 32

typedef struct block
{
    size_t size;
    struct block *next;
} block_t;

static block_t *free_list;
static void (*exit_handlers[ATEXIT_MAX])(void);
static int exit_count;
static unsigned long random_state = 1;

void __knoc_stdio_flush_all(void);

static size_t round_up(size_t value)
{
    return (value + 15) & ~(size_t)15;
}

static void insert_free(block_t *b)
{
    block_t *previous = NULL;
    block_t *current = free_list;

    while (current && current < b)
    {
        previous = current;
        current = current->next;
    }

    b->next = current;

    if (previous)
    {
        previous->next = b;
    }
    else
    {
        free_list = b;
    }

    if (current && (char *)b + b->size == (char *)current)
    {
        b->size += current->size;
        b->next = current->next;
    }

    if (previous && (char *)previous + previous->size == (char *)b)
    {
        previous->size += b->size;
        previous->next = b->next;
    }
}

static int grow(size_t need)
{
    size_t size = need > CHUNK ? round_up(need) : CHUNK;
    block_t *b = mem_alloc(size);

    if (!b)
    {
        return -1;
    }

    b->size = size;
    insert_free(b);
    return 0;
}

void *malloc(size_t size)
{
    if (size == 0)
    {
        size = 1;
    }

    size_t need = round_up(size) + HEADER;

    for (int attempt = 0; attempt < 2; attempt++)
    {
        block_t *previous = NULL;

        for (block_t *b = free_list; b; previous = b, b = b->next)
        {
            if (b->size < need)
            {
                continue;
            }

            if (b->size - need >= HEADER * 2)
            {
                block_t *rest = (block_t *)((char *)b + need);

                rest->size = b->size - need;
                rest->next = b->next;
                b->size = need;
                b->next = rest;
            }

            if (previous)
            {
                previous->next = b->next;
            }
            else
            {
                free_list = b->next;
            }

            b->next = NULL;
            return (char *)b + HEADER;
        }

        if (grow(need) != 0)
        {
            break;
        }
    }

    errno = ENOMEM;
    return NULL;
}

void free(void *pointer)
{
    if (pointer)
    {
        insert_free((block_t *)((char *)pointer - HEADER));
    }
}

void *calloc(size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size)
    {
        errno = ENOMEM;
        return NULL;
    }

    void *memory = malloc(count * size);

    if (memory)
    {
        memset(memory, 0, count * size);
    }

    return memory;
}

void *realloc(void *pointer, size_t size)
{
    if (!pointer)
    {
        return malloc(size);
    }

    if (size == 0)
    {
        free(pointer);
        return NULL;
    }

    block_t *b = (block_t *)((char *)pointer - HEADER);
    size_t have = b->size - HEADER;

    if (have >= size)
    {
        return pointer;
    }

    void *bigger = malloc(size);

    if (bigger)
    {
        memcpy(bigger, pointer, have);
        free(pointer);
    }

    return bigger;
}

static int digit_value(int c)
{
    if (isdigit(c))
    {
        return c - '0';
    }

    if (isalpha(c))
    {
        return tolower(c) - 'a' + 10;
    }

    return 99;
}

unsigned long long strtoull(const char *text, char **end, int base)
{
    const char *p = text;
    int negative = 0;
    int any = 0;
    int overflow = 0;
    unsigned long long value = 0;

    while (isspace((unsigned char)*p))
    {
        p++;
    }

    if (*p == '+' || *p == '-')
    {
        negative = *p++ == '-';
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2]))
    {
        p += 2;
        base = 16;
    }
    else if (base == 0)
    {
        base = p[0] == '0' ? 8 : 10;
    }

    for (; digit_value((unsigned char)*p) < base; p++)
    {
        unsigned long long next = value * (unsigned)base + (unsigned)digit_value((unsigned char)*p);

        if (next / (unsigned)base != value && !overflow)
        {
            overflow = 1;
        }

        value = next;
        any = 1;
    }

    if (end)
    {
        *end = (char *)(any ? p : text);
    }

    if (overflow)
    {
        errno = ERANGE;
        return (unsigned long long)-1;
    }

    return negative ? -value : value;
}

long long strtoll(const char *text, char **end, int base)
{
    const char *p = text;

    while (isspace((unsigned char)*p))
    {
        p++;
    }

    int negative = *p == '-';
    unsigned long long magnitude = strtoull(negative || *p == '+' ? p + 1 : p, end, base);

    if (end && *end == (negative || *p == '+' ? p + 1 : p))
    {
        *end = (char *)text;
    }

    if (!negative && magnitude > (unsigned long long)LLONG_MAX)
    {
        errno = ERANGE;
        return LLONG_MAX;
    }

    if (negative && magnitude > (unsigned long long)LLONG_MAX + 1)
    {
        errno = ERANGE;
        return LLONG_MIN;
    }

    return negative ? -(long long)magnitude : (long long)magnitude;
}

long strtol(const char *text, char **end, int base)
{
    return (long)strtoll(text, end, base);
}

unsigned long strtoul(const char *text, char **end, int base)
{
    return (unsigned long)strtoull(text, end, base);
}

int atoi(const char *text)
{
    return (int)strtol(text, NULL, 10);
}

long atol(const char *text)
{
    return strtol(text, NULL, 10);
}

long long atoll(const char *text)
{
    return strtoll(text, NULL, 10);
}

double strtod(const char *text, char **end)
{
    const char *p = text;
    double value = 0;
    int negative = 0;
    int any = 0;

    while (isspace((unsigned char)*p))
    {
        p++;
    }

    if (*p == '+' || *p == '-')
    {
        negative = *p++ == '-';
    }

    for (; isdigit((unsigned char)*p); p++)
    {
        value = value * 10 + (*p - '0');
        any = 1;
    }

    if (*p == '.')
    {
        double scale = 0.1;

        for (p++; isdigit((unsigned char)*p); p++)
        {
            value += (*p - '0') * scale;
            scale /= 10;
            any = 1;
        }
    }

    if (any && (*p == 'e' || *p == 'E'))
    {
        char *after;
        long exponent = strtol(p + 1, &after, 10);

        if (after != p + 1)
        {
            p = after;

            for (; exponent > 0; exponent--)
            {
                value *= 10;
            }

            for (; exponent < 0; exponent++)
            {
                value /= 10;
            }
        }
    }

    if (end)
    {
        *end = (char *)(any ? p : text);
    }

    return negative ? -value : value;
}

double atof(const char *text)
{
    return strtod(text, NULL);
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

long long llabs(long long value)
{
    return value < 0 ? -value : value;
}

div_t div(int numerator, int denominator)
{
    div_t result = {numerator / denominator, numerator % denominator};

    return result;
}

int rand(void)
{
    random_state = random_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)((random_state >> 33) & RAND_MAX);
}

void srand(unsigned int seed)
{
    random_state = seed;
}

static void swap(char *a, char *b, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        char t = a[i];

        a[i] = b[i];
        b[i] = t;
    }
}

static void sort(char *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    while (count > 12)
    {
        char *middle = base + (count / 2) * size;
        char *last = base + (count - 1) * size;

        if (compare(middle, base) < 0)
        {
            swap(middle, base, size);
        }

        if (compare(last, middle) < 0)
        {
            swap(last, middle, size);

            if (compare(middle, base) < 0)
            {
                swap(middle, base, size);
            }
        }

        swap(middle, last - size, size);

        char *pivot = last - size;
        size_t store = 1;

        for (size_t i = 1; i < count - 2; i++)
        {
            if (compare(base + i * size, pivot) < 0)
            {
                swap(base + i * size, base + store * size, size);
                store++;
            }
        }

        swap(base + store * size, pivot, size);

        if (store < count - store - 1)
        {
            sort(base, store, size, compare);
            base += (store + 1) * size;
            count -= store + 1;
        }
        else
        {
            sort(base + (store + 1) * size, count - store - 1, size, compare);
            count = store;
        }
    }

    for (size_t i = 1; i < count; i++)
    {
        for (size_t j = i; j > 0 && compare(base + (j - 1) * size, base + j * size) > 0; j--)
        {
            swap(base + (j - 1) * size, base + j * size, size);
        }
    }
}

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    if (count > 1 && size > 0)
    {
        sort(base, count, size, compare);
    }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    size_t low = 0;
    size_t high = count;

    while (low < high)
    {
        size_t middle = low + (high - low) / 2;
        const char *item = (const char *)base + middle * size;
        int order = compare(key, item);

        if (order == 0)
        {
            return (void *)item;
        }

        if (order < 0)
        {
            high = middle;
        }
        else
        {
            low = middle + 1;
        }
    }

    return NULL;
}

int atexit(void (*function)(void))
{
    if (exit_count >= ATEXIT_MAX)
    {
        return -1;
    }

    exit_handlers[exit_count++] = function;
    return 0;
}

void exit(int code)
{
    while (exit_count > 0)
    {
        exit_handlers[--exit_count]();
    }

    __knoc_stdio_flush_all();
    syscall(SYS_EXIT, code, 0, 0);

    while (1)
    {
    }
}

void abort(void)
{
    print("abort\n");
    exit(134);
}

char *getenv(const char *name)
{
    if (strcmp(name, "HOME") == 0)
    {
        return "/home";
    }

    if (strcmp(name, "PATH") == 0)
    {
        return "/bin";
    }

    if (strcmp(name, "USER") == 0)
    {
        return "user";
    }

    if (strcmp(name, "OS") == 0)
    {
        return "KnocOS";
    }

    return NULL;
}
