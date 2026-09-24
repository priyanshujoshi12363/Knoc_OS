#include "ulib.h"

#define MODEL_PATH "/models/qwen.kllm"
#define HEADER_BYTES 128
#define MAX_ANSWER 160
#define MAX_PROMPT 700
#define PIECE_MAX 256
#define HASH_BITS 19
#define HASH_SIZE (1u << HASH_BITS)
#define READ_CHUNK (8UL * 1024 * 1024)
#define SYSTEM_PROMPT "You are the KnocOS assistant, built into the KnocOS operating system. Answer briefly and clearly."

typedef struct header
{
    unsigned int version, dim, hidden, layers, heads, kv_heads, vocab, max_seq, group;
    float theta, eps;
    unsigned int endoftext, im_start, im_end, merges, reserved1, reserved2;
    unsigned long tok_off, tok_size, emb_off, emb_size, body_off, body_size;
} header_t;

typedef struct qmatrix
{
    const signed char *q;
    const float *s;
    unsigned int rows, cols;
} qmatrix_t;

typedef struct block
{
    const float *attn_norm;
    qmatrix_t wq, wk, wv, wo, w_gate, w_up, w_down;
    const float *bq, *bk, *bv;
    const float *ffn_norm;
} block_t;

typedef struct merge_slot
{
    unsigned int left, right, rank, result;
} merge_slot_t;

static header_t h;
static unsigned int head_dim, kv_dim, group;
static unsigned char *tok_data, *emb_data, *body_data;
static unsigned int *tok_offset;
static unsigned short *tok_length;
static merge_slot_t *merges;
static int byte_ids[256];
static qmatrix_t embedding;
static block_t *blocks;
static const float *final_norm;

static float *x, *xb, *xb2, *hb, *hb2, *q, *att, *logits, *key_cache, *value_cache, *rope_cos, *rope_sin;
static signed char *xq;
static float *xs;
static int prompt[MAX_PROMPT];

static double exp_fast(double v)
{
    if (v < -700)
    {
        return 0.0;
    }

    if (v > 700)
    {
        v = 700;
    }

    double k = v * 1.4426950408889634;
    long n = (long)(k < 0 ? k - 0.5 : k + 0.5);
    double r = v - (double)n * 0.6931471805599453;
    double p = 1.0 + r * (1.0 + r * (0.5 + r * (1.0 / 6 + r * (1.0 / 24 + r * (1.0 / 120 + r * (1.0 / 720 + r / 5040))))));
    union
    {
        double d;
        unsigned long u;
    } scale;

    scale.u = (unsigned long)(n + 1023) << 52;
    return p * scale.d;
}

static double log_of(double v)
{
    int e = 0;

    while (v > 2.0)
    {
        v /= 2.0;
        e++;
    }

    while (v < 1.0)
    {
        v *= 2.0;
        e--;
    }

    double t = (v - 1.0) / (v + 1.0);
    double t2 = t * t;
    double sum = 0.0;
    double power = t;

    for (int k = 1; k < 40; k += 2)
    {
        sum += power / k;
        power *= t2;
    }

    return e * 0.6931471805599453 + 2.0 * sum;
}

static double sqrt_of(double v)
{
    double r;

    asm("fsqrt.d %0, %1" : "=f"(r) : "f"(v));
    return r;
}

static void sin_cos(double angle, double *s, double *c)
{
    const double two_pi = 6.283185307179586;
    long turns = (long)(angle / two_pi);
    double a = angle - (double)turns * two_pi;

    if (a > 3.141592653589793)
    {
        a -= two_pi;
    }

    double a2 = a * a;
    double sine = a;
    double cosine = 1.0;
    double term_s = a;
    double term_c = 1.0;

    for (int k = 1; k < 14; k++)
    {
        term_s *= -a2 / ((2 * k) * (2 * k + 1));
        term_c *= -a2 / ((2 * k - 1) * (2 * k));
        sine += term_s;
        cosine += term_c;
    }

    *s = sine;
    *c = cosine;
}

static int read_section(int fd, unsigned long offset, unsigned char *to, unsigned long size)
{
    seek(fd, offset);

    for (unsigned long done = 0; done < size;)
    {
        unsigned long want = size - done < READ_CHUNK ? size - done : READ_CHUNK;
        long got = read(fd, to + done, want);

        if (got <= 0)
        {
            return -1;
        }

        done += (unsigned long)got;
    }

    return 0;
}

static unsigned int hash_pair(unsigned int left, unsigned int right)
{
    unsigned long key = ((unsigned long)left << 32) | right;

    key *= 0x9E3779B97F4A7C15UL;
    return (unsigned int)(key >> (64 - HASH_BITS));
}

static void build_tokenizer(void)
{
    unsigned long p = 0;

    for (unsigned int i = 0; i < h.vocab; i++)
    {
        unsigned short length = (unsigned short)(tok_data[p] | (tok_data[p + 1] << 8));

        tok_length[i] = length;
        tok_offset[i] = (unsigned int)(p + 2);
        p += 2 + length;
    }

    p += (64 - p % 64) % 64;

    unsigned int count;

    memcpy(&count, tok_data + p, 4);
    p += 4;

    for (unsigned int i = 0; i < HASH_SIZE; i++)
    {
        merges[i].rank = 0xFFFFFFFFu;
    }

    for (unsigned int rank = 0; rank < count; rank++)
    {
        unsigned int t[3];

        memcpy(t, tok_data + p + (unsigned long)rank * 12, 12);

        unsigned int slot = hash_pair(t[0], t[1]);

        while (merges[slot].rank != 0xFFFFFFFFu)
        {
            slot = (slot + 1) & (HASH_SIZE - 1);
        }

        merges[slot].left = t[0];
        merges[slot].right = t[1];
        merges[slot].rank = rank;
        merges[slot].result = t[2];
    }

    for (int b = 0; b < 256; b++)
    {
        byte_ids[b] = -1;
    }

    for (unsigned int i = 0; i < h.endoftext; i++)
    {
        if (tok_length[i] == 1)
        {
            byte_ids[tok_data[tok_offset[i]]] = (int)i;
        }
    }
}

static int find_merge(unsigned int left, unsigned int right, unsigned int *rank, unsigned int *result)
{
    unsigned int slot = hash_pair(left, right);

    while (merges[slot].rank != 0xFFFFFFFFu)
    {
        if (merges[slot].left == left && merges[slot].right == right)
        {
            *rank = merges[slot].rank;
            *result = merges[slot].result;
            return 1;
        }

        slot = (slot + 1) & (HASH_SIZE - 1);
    }

    return 0;
}

static int encode_piece(const unsigned char *piece, int length, int *out, int room)
{
    int tokens[PIECE_MAX];
    int count = 0;

    for (int i = 0; i < length && count < PIECE_MAX; i++)
    {
        tokens[count++] = byte_ids[piece[i]];
    }

    while (count > 1)
    {
        unsigned int best_rank = 0xFFFFFFFFu;
        unsigned int best_result = 0;
        int best = -1;

        for (int k = 0; k < count - 1; k++)
        {
            unsigned int rank;
            unsigned int result;

            if (find_merge((unsigned int)tokens[k], (unsigned int)tokens[k + 1], &rank, &result) &&
                rank < best_rank)
            {
                best_rank = rank;
                best_result = result;
                best = k;
            }
        }

        if (best < 0)
        {
            break;
        }

        tokens[best] = (int)best_result;

        for (int k = best + 1; k < count - 1; k++)
        {
            tokens[k] = tokens[k + 1];
        }

        count--;
    }

    int written = 0;

    for (int i = 0; i < count && written < room; i++)
    {
        out[written++] = tokens[i];
    }

    return written;
}

static int is_letter(unsigned char b)
{
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b >= 0x80;
}

static int is_digit(unsigned char b)
{
    return b >= '0' && b <= '9';
}

static int is_space(unsigned char b)
{
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

static int is_newline(unsigned char b)
{
    return b == '\n' || b == '\r';
}

static int is_other(unsigned char b)
{
    return !is_space(b) && !is_letter(b) && !is_digit(b);
}

static unsigned char lower_byte(unsigned char b)
{
    return b >= 'A' && b <= 'Z' ? (unsigned char)(b + 32) : b;
}

static int contraction(const unsigned char *d, int i, int n)
{
    static const char *suffixes[] = {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"};

    for (int s = 0; s < 7; s++)
    {
        int length = (int)strlen(suffixes[s]);

        if (i + length > n)
        {
            continue;
        }

        int match = 1;

        for (int k = 0; k < length; k++)
        {
            if (lower_byte(d[i + k]) != (unsigned char)suffixes[s][k])
            {
                match = 0;
                break;
            }
        }

        if (match)
        {
            return length;
        }
    }

    return 0;
}

static int encode(const char *text, int *out, int room)
{
    const unsigned char *d = (const unsigned char *)text;
    int n = (int)strlen(text);
    int i = 0;
    int written = 0;

    while (i < n && written < room)
    {
        unsigned char b = d[i];
        int j;

        if (b == '\'' && i + 1 < n)
        {
            int length = contraction(d, i, n);

            if (length)
            {
                written += encode_piece(d + i, length, out + written, room - written);
                i += length;
                continue;
            }
        }

        if (is_letter(b) || (!is_space(b) && !is_digit(b) && !is_newline(b) && i + 1 < n &&
                             is_letter(d[i + 1]) && !is_letter(b)))
        {
            j = is_letter(b) ? i : i + 1;

            while (j < n && is_letter(d[j]))
            {
                j++;
            }
        }
        else if (is_digit(b))
        {
            j = i + 1;
        }
        else if (is_newline(b))
        {
            j = i;

            while (j < n && is_newline(d[j]))
            {
                j++;
            }
        }
        else if (is_space(b))
        {
            j = i;

            while (j < n && (d[j] == ' ' || d[j] == '\t'))
            {
                j++;
            }

            if (j < n && !is_space(d[j]) && j - i > 1)
            {
                j = j - 1;
            }
            else if (j < n && !is_space(d[j]) && is_other(d[j]))
            {
                while (j < n && is_other(d[j]))
                {
                    j++;
                }

                while (j < n && is_newline(d[j]))
                {
                    j++;
                }
            }
            else if (j == n || j - i > 1 || is_newline(d[j]))
            {
            }
            else
            {
                j = i + 1;

                while (j < n && is_letter(d[j]))
                {
                    j++;
                }
            }
        }
        else
        {
            j = i;

            while (j < n && is_other(d[j]))
            {
                j++;
            }

            while (j < n && is_newline(d[j]))
            {
                j++;
            }
        }

        written += encode_piece(d + i, j - i, out + written, room - written);
        i = j;
    }

    return written;
}

static qmatrix_t take_matrix(unsigned char **p, unsigned int rows, unsigned int cols)
{
    qmatrix_t m;

    m.rows = rows;
    m.cols = cols;
    m.q = (const signed char *)*p;
    *p += (unsigned long)rows * cols;
    m.s = (const float *)*p;
    *p += (unsigned long)rows * cols / group * 4;
    return m;
}

static const float *take_floats(unsigned char **p, unsigned int count)
{
    const float *values = (const float *)*p;

    *p += (unsigned long)count * 4;
    return values;
}

static void quantize_vector(const float *v, unsigned int n)
{
    for (unsigned int g = 0; g < n / group; g++)
    {
        float highest = 0;

        for (unsigned int k = 0; k < group; k++)
        {
            float a = v[g * group + k];

            a = a < 0 ? -a : a;
            highest = a > highest ? a : highest;
        }

        float scale = highest / 127.0f;

        xs[g] = scale;

        for (unsigned int k = 0; k < group; k++)
        {
            float r = scale > 0 ? v[g * group + k] / scale : 0;

            xq[g * group + k] = (signed char)(int)(r + (r >= 0 ? 0.5f : -0.5f));
        }
    }
}

static void matmul(float *out, const qmatrix_t *m, const float *v)
{
    unsigned int groups = m->cols / group;

    quantize_vector(v, m->cols);

    for (unsigned int i = 0; i < m->rows; i++)
    {
        const signed char *row = m->q + (unsigned long)i * m->cols;
        const float *scales = m->s + (unsigned long)i * groups;
        float sum = 0;

        for (unsigned int g = 0; g < groups; g++)
        {
            const long *a = (const long *)(row + g * group);
            const long *b = (const long *)(xq + g * group);
            long dot = 0;

            for (unsigned int k = 0; k < 8; k++)
            {
                long wa = a[k];
                long wb = b[k];

                dot += ((wa << 56) >> 56) * ((wb << 56) >> 56) +
                       ((wa << 48) >> 56) * ((wb << 48) >> 56) +
                       ((wa << 40) >> 56) * ((wb << 40) >> 56) +
                       ((wa << 32) >> 56) * ((wb << 32) >> 56) +
                       ((wa << 24) >> 56) * ((wb << 24) >> 56) +
                       ((wa << 16) >> 56) * ((wb << 16) >> 56) +
                       ((wa << 8) >> 56) * ((wb << 8) >> 56) +
                       (wa >> 56) * (wb >> 56);
            }

            sum += (float)dot * scales[g] * xs[g];
        }

        out[i] = sum;
    }
}

static void rms_norm(float *out, const float *v, const float *weight, unsigned int n)
{
    double sum = 0;

    for (unsigned int i = 0; i < n; i++)
    {
        sum += (double)v[i] * v[i];
    }

    double scale = 1.0 / sqrt_of(sum / n + h.eps);

    for (unsigned int i = 0; i < n; i++)
    {
        out[i] = (float)(v[i] * scale) * weight[i];
    }
}

static void rope(float *v, unsigned int count, unsigned int position)
{
    unsigned int half = head_dim / 2;
    const float *c = rope_cos + (unsigned long)position * half;
    const float *s = rope_sin + (unsigned long)position * half;

    for (unsigned int head = 0; head < count; head++)
    {
        float *p = v + head * head_dim;

        for (unsigned int i = 0; i < half; i++)
        {
            float a = p[i];
            float b = p[i + half];

            p[i] = a * c[i] - b * s[i];
            p[i + half] = b * c[i] + a * s[i];
        }
    }
}

static void forward(int token, unsigned int position, int want_logits)
{
    unsigned int dim = h.dim;
    unsigned int per_kv = h.heads / h.kv_heads;
    const signed char *row = embedding.q + (unsigned long)token * dim;
    const float *scales = embedding.s + (unsigned long)token * (dim / group);

    for (unsigned int i = 0; i < dim; i++)
    {
        x[i] = row[i] * scales[i / group];
    }

    for (unsigned int l = 0; l < h.layers; l++)
    {
        block_t *b = &blocks[l];
        float *keys = key_cache + (unsigned long)l * h.max_seq * kv_dim;
        float *values = value_cache + (unsigned long)l * h.max_seq * kv_dim;
        float *k = keys + (unsigned long)position * kv_dim;
        float *v = values + (unsigned long)position * kv_dim;

        rms_norm(xb, x, b->attn_norm, dim);
        matmul(q, &b->wq, xb);
        matmul(k, &b->wk, xb);
        matmul(v, &b->wv, xb);

        for (unsigned int i = 0; i < dim; i++)
        {
            q[i] += b->bq[i];
        }

        for (unsigned int i = 0; i < kv_dim; i++)
        {
            k[i] += b->bk[i];
            v[i] += b->bv[i];
        }

        rope(q, h.heads, position);
        rope(k, h.kv_heads, position);

        float scale = (float)(1.0 / sqrt_of(head_dim));

        for (unsigned int head = 0; head < h.heads; head++)
        {
            const float *qh = q + head * head_dim;
            unsigned int kv = head / per_kv;
            float highest = -1e30f;

            for (unsigned int t = 0; t <= position; t++)
            {
                const float *kt = keys + (unsigned long)t * kv_dim + kv * head_dim;
                float score = 0;

                for (unsigned int i = 0; i < head_dim; i++)
                {
                    score += qh[i] * kt[i];
                }

                score *= scale;
                att[t] = score;
                highest = score > highest ? score : highest;
            }

            float total = 0;

            for (unsigned int t = 0; t <= position; t++)
            {
                att[t] = (float)exp_fast(att[t] - highest);
                total += att[t];
            }

            float *out = xb2 + head * head_dim;

            for (unsigned int i = 0; i < head_dim; i++)
            {
                out[i] = 0;
            }

            for (unsigned int t = 0; t <= position; t++)
            {
                const float *vt = values + (unsigned long)t * kv_dim + kv * head_dim;
                float weight = att[t] / total;

                for (unsigned int i = 0; i < head_dim; i++)
                {
                    out[i] += weight * vt[i];
                }
            }
        }

        matmul(xb, &b->wo, xb2);

        for (unsigned int i = 0; i < dim; i++)
        {
            x[i] += xb[i];
        }

        rms_norm(xb, x, b->ffn_norm, dim);
        matmul(hb, &b->w_gate, xb);
        matmul(hb2, &b->w_up, xb);

        for (unsigned int i = 0; i < h.hidden; i++)
        {
            float g = hb[i];

            hb[i] = (float)(g / (1.0 + exp_fast(-g))) * hb2[i];
        }

        matmul(xb, &b->w_down, hb);

        for (unsigned int i = 0; i < dim; i++)
        {
            x[i] += xb[i];
        }
    }

    if (want_logits)
    {
        rms_norm(x, x, final_norm, dim);
        matmul(logits, &embedding, x);
    }
}

static void *need(unsigned long bytes)
{
    void *memory = mem_alloc(bytes);

    if (memory == 0)
    {
        print("ask: not enough memory\n");
        exit(1);
    }

    return memory;
}

static int load_model(void)
{
    unsigned char raw[HEADER_BYTES];
    int fd = open(MODEL_PATH, O_READ);

    if (fd < 0 || read(fd, raw, HEADER_BYTES) != HEADER_BYTES || memcmp_bytes(raw, "KNOCLLM1", 8) != 0)
    {
        return -1;
    }

    memcpy(&h, raw + 8, 17 * 4);
    memcpy(&h.tok_off, raw + 8 + 17 * 4, 6 * 8);
    head_dim = h.dim / h.heads;
    kv_dim = h.kv_heads * head_dim;
    group = h.group;

    tok_data = need(h.tok_size);
    emb_data = need(h.emb_size);
    body_data = need(h.body_size);

    if (read_section(fd, h.tok_off, tok_data, h.tok_size) != 0 ||
        read_section(fd, h.emb_off, emb_data, h.emb_size) != 0 ||
        read_section(fd, h.body_off, body_data, h.body_size) != 0)
    {
        return -1;
    }

    close(fd);

    tok_offset = need((unsigned long)h.vocab * 4);
    tok_length = need((unsigned long)h.vocab * 2);
    merges = need((unsigned long)HASH_SIZE * sizeof(merge_slot_t));
    build_tokenizer();

    unsigned char *p = emb_data;

    embedding = take_matrix(&p, h.vocab, h.dim);
    blocks = need(h.layers * sizeof(block_t));
    p = body_data;

    for (unsigned int l = 0; l < h.layers; l++)
    {
        block_t *b = &blocks[l];

        b->attn_norm = take_floats(&p, h.dim);
        b->wq = take_matrix(&p, h.dim, h.dim);
        b->bq = take_floats(&p, h.dim);
        b->wk = take_matrix(&p, kv_dim, h.dim);
        b->bk = take_floats(&p, kv_dim);
        b->wv = take_matrix(&p, kv_dim, h.dim);
        b->bv = take_floats(&p, kv_dim);
        b->wo = take_matrix(&p, h.dim, h.dim);
        b->ffn_norm = take_floats(&p, h.dim);
        b->w_gate = take_matrix(&p, h.hidden, h.dim);
        b->w_up = take_matrix(&p, h.hidden, h.dim);
        b->w_down = take_matrix(&p, h.dim, h.hidden);
    }

    final_norm = take_floats(&p, h.dim);

    unsigned long width = h.hidden > h.dim ? h.hidden : h.dim;
    unsigned long runtime = (unsigned long)(h.dim * 4 + h.hidden * 2 + h.max_seq + h.vocab) * 4 +
                            width + width / group * 4;

    float *arena = need(runtime + 64);

    x = arena;
    xb = x + h.dim;
    xb2 = xb + h.dim;
    q = xb2 + h.dim;
    hb = q + h.dim;
    hb2 = hb + h.hidden;
    att = hb2 + h.hidden;
    logits = att + h.max_seq;
    xs = logits + h.vocab;
    xq = (signed char *)(xs + width / group);

    unsigned long cache = (unsigned long)h.layers * h.max_seq * kv_dim;

    key_cache = need(cache * 4);
    value_cache = need(cache * 4);

    unsigned int half = head_dim / 2;

    rope_cos = need((unsigned long)h.max_seq * half * 4);
    rope_sin = need((unsigned long)h.max_seq * half * 4);

    double log_theta = log_of(h.theta);

    for (unsigned int i = 0; i < half; i++)
    {
        double frequency = exp_fast(-log_theta * (2.0 * i / head_dim));

        for (unsigned int position = 0; position < h.max_seq; position++)
        {
            double s;
            double c;

            sin_cos(position * frequency, &s, &c);
            rope_sin[(unsigned long)position * half + i] = (float)s;
            rope_cos[(unsigned long)position * half + i] = (float)c;
        }
    }

    return 0;
}

static int add(int *ids, int count, const char *text)
{
    return count + encode(text, ids + count, MAX_PROMPT - count);
}

int main(void)
{
    char question[ARGS_MAX];
    unsigned long started = uptime();

    getargs(question, sizeof(question));

    if (question[0] == 0)
    {
        print("usage: ask QUESTION\n");
        return 1;
    }

    if (load_model() != 0)
    {
        print("ask: cannot load " MODEL_PATH " (make reset-disk DISK_MB=1024 puts it on the disk)\n");
        return 1;
    }

    int count = 0;

    prompt[count++] = (int)h.im_start;
    count = add(prompt, count, "system\n" SYSTEM_PROMPT);
    prompt[count++] = (int)h.im_end;
    count = add(prompt, count, "\n");
    prompt[count++] = (int)h.im_start;
    count = add(prompt, count, "user\n");
    count = add(prompt, count, question);
    prompt[count++] = (int)h.im_end;
    count = add(prompt, count, "\n");
    prompt[count++] = (int)h.im_start;
    count = add(prompt, count, "assistant\n");

    unsigned long loaded = uptime();

    print("[ask] Qwen2.5-0.5B loaded in ");
    print_uint((loaded - started) / 100);
    print(" s, reading ");
    print_uint((unsigned long)count);
    print(" prompt tokens...\n");

    for (int i = 0; i < count; i++)
    {
        forward(prompt[i], (unsigned int)i, i == count - 1);
    }

    unsigned long thought = uptime();
    unsigned int position = (unsigned int)count;
    int produced = 0;

    while (produced < MAX_ANSWER && position < h.max_seq)
    {
        int best = 0;

        for (unsigned int i = 1; i < h.vocab; i++)
        {
            if (logits[i] > logits[best])
            {
                best = (int)i;
            }
        }

        if ((unsigned int)best == h.im_end || (unsigned int)best == h.endoftext)
        {
            break;
        }

        write(FD_STDOUT, tok_data + tok_offset[best], tok_length[best]);
        produced++;
        forward(best, position++, 1);
    }

    unsigned long finished = uptime();

    print("\n[ask] ");
    print_uint((unsigned long)produced);
    print(" tokens in ");
    print_uint((finished - thought) / 100);
    print(" s (prompt ");
    print_uint((thought - loaded) / 100);
    print(" s)\n");
    return 0;
}
