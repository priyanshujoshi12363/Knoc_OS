#include "ulib.h"
#include "learn.h"

#define EXAMPLES_PATH LEARN_DIR "/examples.bin"
#define MODEL_PATH LEARN_DIR "/personal.bin"
#define EXAMPLES_MAX 200
#define FOLDERS_MAX 15
#define CLASSES (FOLDERS_MAX + 1)
#define EPOCHS 60
#define RATE 0.3
#define DECAY 0.0001
#define MIN_CONFIDENCE 0.75
#define MAGIC "KNOCPRS1"

typedef struct example
{
    char name[64];
    char folder[PATH_MAX];
    unsigned long size;
    int type;
    unsigned char x[LEARN_DIM];
} example_t;

typedef struct personal
{
    char magic[8];
    unsigned int dim;
    unsigned int folders;
    char names[FOLDERS_MAX][PATH_MAX];
    unsigned int types[FOLDERS_MAX];
    float weights[CLASSES][LEARN_DIM + 1];
} personal_t;

static example_t examples[EXAMPLES_MAX];
static int example_count;
static int examples_loaded;
static personal_t model;
static int model_ready;
static double input[LEARN_DIM];

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int is_letter(char c)
{
    c = lower(c);
    return c >= 'a' && c <= 'z';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static unsigned int hash_word(const char *word, int length)
{
    unsigned int h = 2166136261u;

    for (int i = 0; i < length; i++)
    {
        h ^= (unsigned char)lower(word[i]);
        h *= 16777619u;
    }

    return h % LEARN_NAME_BINS;
}

void learn_features(const nn_model_t *model_in, const char *name, double *x)
{
    double hidden[256];
    unsigned int width = nn_hidden(model_in, hidden);

    for (int i = 0; i < LEARN_DIM; i++)
    {
        x[i] = 0;
    }

    for (unsigned int i = 0; i < width && i < LEARN_HIDDEN; i++)
    {
        x[i] = hidden[i];
    }

    for (int i = 0; name[i];)
    {
        int j = i;

        if (is_letter(name[i]))
        {
            while (is_letter(name[j]))
            {
                j++;
            }

            if (j - i >= 2)
            {
                x[LEARN_HIDDEN + hash_word(name + i, j - i)] = 1;
            }
        }
        else if (is_digit(name[i]))
        {
            char shape[8] = {'#', 'd', 0};

            while (is_digit(name[j]))
            {
                j++;
            }

            shape[2] = (char)('0' + (j - i > 9 ? 9 : j - i));
            x[LEARN_HIDDEN + hash_word(shape, 3)] = 1;
        }
        else
        {
            j++;
        }

        i = j;
    }
}

static void save_examples(void)
{
    mkdir(LEARN_DIR);

    int fd = open(EXAMPLES_PATH, O_WRITE | O_CREATE | O_TRUNC);

    if (fd < 0)
    {
        return;
    }

    write(fd, &example_count, sizeof(example_count));
    write(fd, examples, (unsigned long)example_count * sizeof(example_t));
    close(fd);
}

static void load_examples(void)
{
    if (examples_loaded)
    {
        return;
    }

    examples_loaded = 1;
    example_count = 0;

    int fd = open(EXAMPLES_PATH, O_READ);

    if (fd < 0)
    {
        return;
    }

    int count = 0;

    if (read(fd, &count, sizeof(count)) == sizeof(count) && count > 0 && count <= EXAMPLES_MAX &&
        read(fd, examples, (unsigned long)count * sizeof(example_t)) == (long)((unsigned long)count * sizeof(example_t)))
    {
        example_count = count;
    }

    close(fd);
}

int learn_load(void)
{
    int fd = open(MODEL_PATH, O_READ);

    model_ready = 0;

    if (fd < 0)
    {
        return -1;
    }

    long got = read(fd, &model, sizeof(model));

    close(fd);
    model_ready = got == (long)sizeof(model) && memcmp_bytes(model.magic, MAGIC, 8) == 0 &&
                  model.dim == LEARN_DIM && model.folders > 0 && model.folders <= FOLDERS_MAX;
    return model_ready ? 0 : -1;
}

static void scores(const double *x, unsigned int classes, double *p)
{
    double highest = -1e30;

    for (unsigned int c = 0; c < classes; c++)
    {
        double z = model.weights[c][LEARN_DIM];

        for (int i = 0; i < LEARN_DIM; i++)
        {
            z += model.weights[c][i] * x[i];
        }

        p[c] = z;
        highest = z > highest ? z : highest;
    }

    double total = 0;

    for (unsigned int c = 0; c < classes; c++)
    {
        p[c] = nn_exp(p[c] - highest);
        total += p[c];
    }

    for (unsigned int c = 0; c < classes; c++)
    {
        p[c] /= total;
    }
}

int learn_predict(const double *x, int type, char *folder, double *confidence)
{
    double p[CLASSES];

    if (!model_ready)
    {
        return 0;
    }

    scores(x, model.folders + 1, p);

    unsigned int best = 0;

    for (unsigned int c = 1; c <= model.folders; c++)
    {
        if (p[c] > p[best])
        {
            best = c;
        }
    }

    if (best == 0 || p[best] < MIN_CONFIDENCE || !(model.types[best - 1] & (1u << type)))
    {
        return 0;
    }

    strcpy(folder, model.names[best - 1]);
    *confidence = p[best];
    return 1;
}

int learn_add(const char *name, unsigned long size, const char *folder, int type, const double *x)
{
    load_examples();

    int slot = -1;

    for (int i = 0; i < example_count; i++)
    {
        if (examples[i].size == size && strcmp(examples[i].name, name) == 0)
        {
            if (strcmp(examples[i].folder, folder) == 0)
            {
                return 0;
            }

            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        if (example_count < EXAMPLES_MAX)
        {
            slot = example_count++;
        }
        else
        {
            for (int i = 1; i < EXAMPLES_MAX; i++)
            {
                examples[i - 1] = examples[i];
            }

            slot = EXAMPLES_MAX - 1;
        }
    }

    example_t *e = &examples[slot];

    memset(e, 0, sizeof(*e));

    for (int i = 0; name[i] && i < (int)sizeof(e->name) - 1; i++)
    {
        e->name[i] = name[i];
    }

    for (int i = 0; folder[i] && i < PATH_MAX - 1; i++)
    {
        e->folder[i] = folder[i];
    }

    e->size = size;
    e->type = type;

    for (int i = 0; i < LEARN_DIM; i++)
    {
        double v = x[i] < 0 ? 0 : x[i] > 1 ? 1 : x[i];

        e->x[i] = (unsigned char)(v * 255 + 0.5);
    }

    save_examples();
    return 1;
}

static int folder_index(const char *folder)
{
    if (folder[0] == 0)
    {
        return 0;
    }

    for (unsigned int i = 0; i < model.folders; i++)
    {
        if (strcmp(model.names[i], folder) == 0)
        {
            return (int)i + 1;
        }
    }

    if (model.folders >= FOLDERS_MAX)
    {
        return -1;
    }

    strcpy(model.names[model.folders], folder);
    model.types[model.folders] = 0;
    model.folders++;
    return (int)model.folders;
}

int learn_train(void)
{
    int labels[EXAMPLES_MAX];
    double counts[CLASSES];

    load_examples();
    memset(&model, 0, sizeof(model));
    memcpy(model.magic, MAGIC, 8);
    model.dim = LEARN_DIM;

    for (int i = 0; i < example_count; i++)
    {
        labels[i] = folder_index(examples[i].folder);

        if (labels[i] > 0)
        {
            model.types[labels[i] - 1] |= 1u << examples[i].type;
        }
    }

    if (model.folders == 0)
    {
        remove(MODEL_PATH);
        model_ready = 0;
        return 0;
    }

    unsigned int classes = model.folders + 1;

    for (unsigned int c = 0; c < classes; c++)
    {
        counts[c] = 0;
    }

    for (int i = 0; i < example_count; i++)
    {
        if (labels[i] >= 0)
        {
            counts[labels[i]] += 1;
        }
    }

    for (int epoch = 0; epoch < EPOCHS; epoch++)
    {
        for (int i = 0; i < example_count; i++)
        {
            double p[CLASSES];

            if (labels[i] < 0)
            {
                continue;
            }

            for (int d = 0; d < LEARN_DIM; d++)
            {
                input[d] = examples[i].x[d] / 255.0;
            }

            scores(input, classes, p);

            double balance = (double)example_count / (classes * counts[labels[i]]);

            for (unsigned int c = 0; c < classes; c++)
            {
                double g = (p[c] - (c == (unsigned int)labels[i] ? 1.0 : 0.0)) * balance * RATE;

                for (int d = 0; d < LEARN_DIM; d++)
                {
                    model.weights[c][d] -= (float)(g * input[d] + DECAY * model.weights[c][d]);
                }

                model.weights[c][LEARN_DIM] -= (float)g;
            }
        }
    }

    mkdir(LEARN_DIR);

    int fd = open(MODEL_PATH, O_WRITE | O_CREATE | O_TRUNC);

    if (fd >= 0)
    {
        write(fd, &model, sizeof(model));
        close(fd);
    }

    model_ready = 1;
    return (int)model.folders;
}

void learn_forget(void)
{
    remove(EXAMPLES_PATH);
    remove(MODEL_PATH);
    remove(LEARN_DIR);
    example_count = 0;
    examples_loaded = 1;
    model_ready = 0;
}

void learn_list(void)
{
    int shown = 0;

    load_examples();

    for (int i = 0; i < example_count; i++)
    {
        if (examples[i].folder[0] == 0)
        {
            continue;
        }

        if (!shown)
        {
            print("Learned from your corrections:\n");
            shown = 1;
        }

        print("  ");
        print(examples[i].name);
        print("  ->  ");
        print(examples[i].folder);
        print("\n");
    }

    if (!shown)
    {
        print("Nothing learned yet: move a file organize sorted into your own folder, then run organize learn\n");
    }

    print_uint((unsigned long)example_count);
    print(" examples stored in " LEARN_DIR "\n");
}
