#include "ulib.h"
#include "llm.h"

#define MODEL_CHOICE "/etc/llm.model"
#define CONTEXT 2048
#define HEADER_BYTES 128
#define PIECE_MAX 256
#define HASH_BITS 19
#define HASH_SIZE (1u << HASH_BITS)
#define READ_CHUNK (8UL * 1024 * 1024)
#define PREFIX_MAGIC "KNOCKV02"
#define WORKERS_MAX 3
#define WORKER_STACK (64UL * 1024)
#define WORKER_SPINS 4000000UL
#define PARALLEL_ROWS 64

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
static unsigned int position;
static unsigned char model_header[HEADER_BYTES];
static char model_path[96];

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

static int encode_plain(const char *text, int n, int *out, int room)
{
    const unsigned char *d = (const unsigned char *)text;
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

static int special_at(const char *text, int left, int *length)
{
    for (unsigned int id = h.endoftext; id < h.vocab; id++)
    {
        unsigned int size = tok_length[id];

        if (size > 0 && (int)size <= left && memcmp_bytes(text, tok_data + tok_offset[id], size) == 0)
        {
            *length = (int)size;
            return (int)id;
        }
    }

    return -1;
}

static int encode(const char *text, int *out, int room)
{
    int n = (int)strlen(text);
    int start = 0;
    int written = 0;

    for (int i = 0; i <= n && written < room; i++)
    {
        int length = 0;
        int id = i < n && text[i] == '<' ? special_at(text + i, n - i, &length) : -1;

        if (id < 0 && i < n)
        {
            continue;
        }

        written += encode_plain(text + start, i - start, out + written, room - written);

        if (id >= 0 && written < room)
        {
            out[written++] = id;
            i += length - 1;
        }

        start = i + 1;
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

static void quantize_vector(const float *v, unsigned int n, signed char *q_out, float *s_out)
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

        s_out[g] = scale;

        for (unsigned int k = 0; k < group; k++)
        {
            float r = scale > 0 ? v[g * group + k] / scale : 0;

            q_out[g * group + k] = (signed char)(int)(r + (r >= 0 ? 0.5f : -0.5f));
        }
    }
}

#define LANE(w, n) (((w) << (56 - 8 * (n))) >> 56)

static unsigned int workers;
static volatile unsigned int job_generation;
static volatile unsigned int job_done;
static float *job_out;
static const qmatrix_t *job_matrix;

static void matmul_rows(float *out, const qmatrix_t *m, unsigned int from, unsigned int to)
{
    unsigned int groups = m->cols / group;

    for (unsigned int i = from; i < to; i++)
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

                dot += LANE(wa, 0) * LANE(wb, 0) + LANE(wa, 1) * LANE(wb, 1) + LANE(wa, 2) * LANE(wb, 2) +
                       LANE(wa, 3) * LANE(wb, 3) + LANE(wa, 4) * LANE(wb, 4) + LANE(wa, 5) * LANE(wb, 5) +
                       LANE(wa, 6) * LANE(wb, 6) + (wa >> 56) * (wb >> 56);
            }

            sum += (float)dot * scales[g] * xs[g];
        }

        out[i] = sum;
    }
}

static unsigned int slice_start(unsigned int rows, unsigned int part)
{
    return (unsigned int)((unsigned long)rows * part / (workers + 1));
}

static void matmul_worker(void *argument)
{
    unsigned int part = (unsigned int)(unsigned long)argument;
    unsigned int seen = 0;
    unsigned long spins = 0;

    while (1)
    {
        unsigned int generation = __atomic_load_n(&job_generation, __ATOMIC_ACQUIRE);

        if (generation == seen)
        {
            if (++spins > WORKER_SPINS)
            {
                sleep(1);
                spins = 0;
            }

            continue;
        }

        seen = generation;
        spins = 0;
        matmul_rows(job_out, job_matrix, slice_start(job_matrix->rows, part),
                    slice_start(job_matrix->rows, part + 1));
        __atomic_fetch_add(&job_done, 1, __ATOMIC_RELEASE);
    }
}

static void start_workers(void)
{
    cpu_info_t info;
    unsigned int ai_cores = 0;

    for (unsigned long i = 0; cpuinfo(i, &info) == 0; i++)
    {
        ai_cores += info.online && info.role == CPU_ROLE_AI;
    }

    unsigned int wanted = ai_cores > 1 ? ai_cores - 1 : 0;

    if (wanted > WORKERS_MAX)
    {
        wanted = WORKERS_MAX;
    }

    for (unsigned int i = 0; i < wanted; i++)
    {
        void *stack = mem_alloc(WORKER_STACK);

        if (!stack || thread_spawn(matmul_worker, (void *)(unsigned long)(i + 1), stack, WORKER_STACK) < 0)
        {
            break;
        }

        workers++;
    }
}

static void matmul(float *out, const qmatrix_t *m, const float *in)
{
    quantize_vector(in, m->cols, xq, xs);

    if (workers == 0 || m->rows < PARALLEL_ROWS)
    {
        matmul_rows(out, m, 0, m->rows);
        return;
    }

    job_out = out;
    job_matrix = m;
    __atomic_store_n(&job_done, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&job_generation, 1, __ATOMIC_RELEASE);
    matmul_rows(out, m, 0, slice_start(m->rows, 1));

    while (__atomic_load_n(&job_done, __ATOMIC_ACQUIRE) != workers)
    {
    }
}

unsigned int llm_threads(void)
{
    return workers + 1;
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

static void attend(const float *qt, float *out, const float *keys, const float *values, unsigned int position)
{
    unsigned int per_kv = h.heads / h.kv_heads;
    float scale = (float)(1.0 / sqrt_of(head_dim));

    for (unsigned int head = 0; head < h.heads; head++)
    {
        const float *qh = qt + head * head_dim;
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

        float *oh = out + head * head_dim;

        for (unsigned int i = 0; i < head_dim; i++)
        {
            oh[i] = 0;
        }

        for (unsigned int t = 0; t <= position; t++)
        {
            const float *vt = values + (unsigned long)t * kv_dim + kv * head_dim;
            float weight = att[t] / total;

            for (unsigned int i = 0; i < head_dim; i++)
            {
                oh[i] += weight * vt[i];
            }
        }
    }
}

static void forward(int token, unsigned int position, int want_logits)
{
    unsigned int dim = h.dim;
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
        attend(q, xb2, keys, values, position);
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
        print("llm: not enough memory\n");
        exit(1);
    }

    return memory;
}

static void choose_model(void)
{
    int fd = open(MODEL_CHOICE, O_READ);
    long got = fd < 0 ? 0 : read(fd, model_path, sizeof(model_path) - 1);

    if (fd >= 0)
    {
        close(fd);
    }

    while (got > 0 && (model_path[got - 1] == '\n' || model_path[got - 1] == ' ' || model_path[got - 1] == '\r'))
    {
        got--;
    }

    if (got <= 0 || model_path[0] != '/')
    {
        strcpy(model_path, LLM_MODEL_PATH);
        return;
    }

    model_path[got] = 0;
}

const char *llm_model_path(void)
{
    return model_path[0] ? model_path : LLM_MODEL_PATH;
}

int llm_load(void)
{
    unsigned char *raw = model_header;

    choose_model();

    int fd = open(model_path, O_READ);

    if (fd < 0 || read(fd, raw, HEADER_BYTES) != HEADER_BYTES || memcmp_bytes(raw, "KNOCLLM1", 8) != 0)
    {
        return -1;
    }

    memcpy(&h, raw + 8, 17 * 4);
    memcpy(&h.tok_off, raw + 8 + 17 * 4, 6 * 8);
    h.max_seq = CONTEXT;
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

    start_workers();
    return 0;
}

static int prefix_cached(const char *path, const int *ids, int count)
{
    char magic[8];
    unsigned int stored;
    static int stored_ids[LLM_MAX_PROMPT];
    static unsigned char stored_header[HEADER_BYTES];
    int fd = open(path, O_READ);

    if (fd < 0)
    {
        return 0;
    }

    int ok = read(fd, magic, 8) == 8 && memcmp_bytes(magic, PREFIX_MAGIC, 8) == 0 &&
             read(fd, stored_header, HEADER_BYTES) == HEADER_BYTES &&
             memcmp_bytes(stored_header, model_header, HEADER_BYTES) == 0 &&
             read(fd, &stored, 4) == 4 && stored == (unsigned int)count &&
             read(fd, stored_ids, (unsigned long)count * 4) == (long)count * 4 &&
             memcmp_bytes(stored_ids, ids, (unsigned long)count * 4) == 0;
    unsigned long bytes = (unsigned long)count * kv_dim * 4;

    for (unsigned int l = 0; ok && l < h.layers; l++)
    {
        unsigned long offset = (unsigned long)l * h.max_seq * kv_dim;

        ok = read(fd, key_cache + offset, bytes) == (long)bytes && read(fd, value_cache + offset, bytes) == (long)bytes;
    }

    close(fd);
    return ok;
}

static void save_prefix(const char *path, const int *ids, int count)
{
    unsigned int stored = (unsigned int)count;
    unsigned long bytes = (unsigned long)count * kv_dim * 4;
    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);

    if (fd < 0)
    {
        return;
    }

    write(fd, PREFIX_MAGIC, 8);
    write(fd, model_header, HEADER_BYTES);
    write(fd, &stored, 4);
    write(fd, ids, (unsigned long)count * 4);

    for (unsigned int l = 0; l < h.layers; l++)
    {
        unsigned long offset = (unsigned long)l * h.max_seq * kv_dim;

        write(fd, key_cache + offset, bytes);
        write(fd, value_cache + offset, bytes);
    }

    close(fd);
}

static void prompt_text(llm_prompt_t *p, const char *text)
{
    p->count += encode(text, p->ids + p->count, LLM_MAX_PROMPT - 4 - p->count);
}

void llm_prompt_start(llm_prompt_t *p, const char *system)
{
    p->count = 0;
    p->ids[p->count++] = (int)h.im_start;
    prompt_text(p, "system\n");
    prompt_text(p, system);
    p->ids[p->count++] = (int)h.im_end;
    prompt_text(p, "\n");
    p->ids[p->count++] = (int)h.im_start;
    prompt_text(p, "user\n");
}

void llm_prompt_text(llm_prompt_t *p, const char *text)
{
    prompt_text(p, text);
}

void llm_prompt_end(llm_prompt_t *p, const char *answer_start)
{
    p->ids[p->count++] = (int)h.im_end;
    prompt_text(p, "\n");
    p->ids[p->count++] = (int)h.im_start;
    prompt_text(p, "assistant\n");

    if (answer_start)
    {
        prompt_text(p, answer_start);
    }
}

void llm_read_prompt(const llm_prompt_t *p, int prefix, const char *cache_path)
{
    int first = 0;

    if (cache_path && prefix > 0 && prefix_cached(cache_path, p->ids, prefix))
    {
        first = prefix;
    }

    for (int i = first; i < p->count; i++)
    {
        forward(p->ids[i], (unsigned int)i, i == p->count - 1);

        if (cache_path && first == 0 && i == prefix - 1)
        {
            save_prefix(cache_path, p->ids, prefix);
        }
    }

    position = (unsigned int)p->count;
}

int llm_best_token(llm_allow_t allow, void *context)
{
    int best = -1;

    for (unsigned int i = 0; i < h.vocab; i++)
    {
        if ((best < 0 || logits[i] > logits[best]) &&
            (allow == 0 || allow((int)i, tok_data + tok_offset[i], tok_length[i], context)))
        {
            best = (int)i;
        }
    }

    return best;
}

int llm_is_end(int token)
{
    return (unsigned int)token == h.im_end || (unsigned int)token == h.endoftext;
}

int llm_accept(int token)
{
    if (position >= h.max_seq)
    {
        return -1;
    }

    forward(token, position++, 1);
    return 0;
}

const unsigned char *llm_token_text(int token, unsigned int *length)
{
    *length = tok_length[token];
    return tok_data + tok_offset[token];
}

static void feed(const int *ids, int count)
{
    for (int i = 0; i < count && position < h.max_seq; i++)
    {
        forward(ids[i], position++, i == count - 1);
    }
}

int llm_feed_text(const char *text)
{
    static int ids[LLM_MAX_PROMPT];
    int count = encode(text, ids, LLM_MAX_PROMPT);

    feed(ids, count);
    return position < h.max_seq ? 0 : -1;
}

int llm_feed_special(int which)
{
    int id = which == LLM_IM_START ? (int)h.im_start : (int)h.im_end;

    feed(&id, 1);
    return position < h.max_seq ? 0 : -1;
}

unsigned int llm_room(void)
{
    return position < h.max_seq ? h.max_seq - position : 0;
}
