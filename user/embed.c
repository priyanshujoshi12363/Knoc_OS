#include "ulib.h"
#include "embed.h"

#define HEADER 40
#define WORD_MAX 32

static unsigned int fnv(unsigned char prefix, const unsigned char *data, unsigned int length)
{
    unsigned int value = 0x811C9DC5u;

    value ^= prefix;
    value *= 0x01000193u;

    for (unsigned int i = 0; i < length; i++)
    {
        value ^= data[i];
        value *= 0x01000193u;
    }

    return value;
}

static float square_root(float value)
{
    float result;

    asm volatile("fsqrt.s %0, %1" : "=f"(result) : "f"(value));
    return result;
}

static long read_all(int fd, void *buffer, unsigned long length)
{
    unsigned char *p = buffer;
    unsigned long done = 0;

    while (done < length)
    {
        long got = syscall(SYS_READ, fd, (long)(p + done), (long)(length - done));

        if (got <= 0)
        {
            break;
        }

        done += (unsigned long)got;
    }

    return (long)done;
}

int embed_load(embed_model_t *model, const char *path)
{
    unsigned char header[HEADER];
    int fd = (int)syscall(SYS_OPEN, (long)path, O_READ, 0);

    if (fd < 0)
    {
        return -1;
    }

    if (read_all(fd, header, HEADER) != HEADER || memcmp_bytes(header, "KNEMBED1", 8) != 0)
    {
        syscall(SYS_CLOSE, fd, 0, 0);
        return -1;
    }

    unsigned int fields[6];

    memcpy(fields, header + 8, sizeof(fields));
    model->buckets = fields[0];
    model->dim = fields[1];
    model->min_n = fields[2];
    model->max_n = fields[3];
    model->max_words = fields[4];
    model->max_word = fields[5];

    if (model->dim == 0 || model->dim > EMBED_DIM_MAX || model->buckets == 0 || model->max_word > WORD_MAX)
    {
        syscall(SYS_CLOSE, fd, 0, 0);
        return -1;
    }

    unsigned long scale_bytes = (unsigned long)model->buckets * 4;
    unsigned long table_bytes = (unsigned long)model->buckets * model->dim;

    model->scales = mem_alloc(scale_bytes);
    model->table = mem_alloc(table_bytes);

    int ok = model->scales && model->table && read_all(fd, model->scales, scale_bytes) == (long)scale_bytes &&
             read_all(fd, model->table, table_bytes) == (long)table_bytes;

    syscall(SYS_CLOSE, fd, 0, 0);
    return ok ? 0 : -1;
}

typedef struct word_reader
{
    const unsigned char *text;
    unsigned long length;
    unsigned long position;
} word_reader_t;

static int next_word(word_reader_t *r, unsigned char *word, unsigned int max)
{
    unsigned int n = 0;
    int previous = 0;

    while (r->position < r->length)
    {
        unsigned char byte = r->text[r->position];
        int kind;
        unsigned char value = byte;

        if (byte >= 'A' && byte <= 'Z')
        {
            kind = 1;
            value = (unsigned char)(byte + 32);
        }
        else if ((byte >= 'a' && byte <= 'z') || byte >= 128)
        {
            kind = 2;
        }
        else if (byte >= '0' && byte <= '9')
        {
            kind = 3;
        }
        else
        {
            r->position++;

            if (n > 0)
            {
                return (int)n;
            }

            previous = 0;
            continue;
        }

        if (n > 0 && ((previous == 3) != (kind == 3) || (previous == 2 && kind == 1)))
        {
            return (int)n;
        }

        if (n < max)
        {
            word[n] = value;
        }

        n++;
        r->position++;
        previous = kind;
    }

    return (int)n;
}

static void add_row(const embed_model_t *m, unsigned int bucket, float *sum)
{
    const signed char *row = m->table + (unsigned long)bucket * m->dim;
    float scale = m->scales[bucket];

    for (unsigned int i = 0; i < m->dim; i++)
    {
        sum[i] += row[i] * scale;
    }
}

int embed_text(const embed_model_t *m, const char *text, unsigned long length, float *out)
{
    word_reader_t reader = {(const unsigned char *)text, length, 0};
    unsigned char word[WORD_MAX + 2];
    float word_sum[EMBED_DIM_MAX];
    int words = 0;

    for (unsigned int i = 0; i < m->dim; i++)
    {
        out[i] = 0;
    }

    while ((unsigned int)words < m->max_words)
    {
        int n = next_word(&reader, word + 1, m->max_word);

        if (n == 0)
        {
            break;
        }

        unsigned int length_used = (unsigned int)n < m->max_word ? (unsigned int)n : m->max_word;
        int digits = 1;

        for (unsigned int i = 1; i <= length_used; i++)
        {
            digits &= word[i] >= '0' && word[i] <= '9';
        }

        for (unsigned int i = 0; i < m->dim; i++)
        {
            word_sum[i] = 0;
        }

        unsigned int features = 1;

        add_row(m, fnv('W', word + 1, length_used) % m->buckets, word_sum);

        if (!digits)
        {
            word[0] = '<';
            word[length_used + 1] = '>';

            unsigned int wrapped = length_used + 2;

            for (unsigned int size = m->min_n; size <= m->max_n; size++)
            {
                for (unsigned int start = 0; start + size <= wrapped; start++)
                {
                    add_row(m, fnv('C', word + start, size) % m->buckets, word_sum);
                    features++;
                }
            }
        }

        for (unsigned int i = 0; i < m->dim; i++)
        {
            out[i] += word_sum[i] / (float)features;
        }

        words++;
    }

    float norm = 0;

    for (unsigned int i = 0; i < m->dim; i++)
    {
        norm += out[i] * out[i];
    }

    if (norm > 0)
    {
        norm = square_root(norm);

        for (unsigned int i = 0; i < m->dim; i++)
        {
            out[i] /= norm;
        }
    }

    return words;
}

float embed_dot(const float *a, const float *b, unsigned int dim)
{
    float sum = 0;

    for (unsigned int i = 0; i < dim; i++)
    {
        sum += a[i] * b[i];
    }

    return sum;
}

int embed_words(const char *text, unsigned long length, char words[][32], int max)
{
    word_reader_t reader = {(const unsigned char *)text, length, 0};
    unsigned char word[WORD_MAX];
    int count = 0;

    while (count < max)
    {
        int n = next_word(&reader, word, WORD_MAX - 1);

        if (n == 0)
        {
            break;
        }

        int used = n < WORD_MAX - 1 ? n : WORD_MAX - 1;

        for (int i = 0; i < used; i++)
        {
            words[count][i] = (char)word[i];
        }

        words[count][used] = 0;
        count++;
    }

    return count;
}
