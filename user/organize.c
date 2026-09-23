#include "ulib.h"

#define MODEL_PATH "/models/filenet.knm"
#define DEFAULT_DIR "/home/Downloads"
#define LOG_NAME ".organize-log"
#define HEAD_BYTES 512
#define FILES_MAX 128
#define LAYERS_MAX 4
#define HEADS_MAX 2
#define CLASSES_MAX 16
#define WIDTH_MAX 512
#define MIN_CONFIDENCE 0.9

#define HIST 256
#define PAIRS 512
#define MAGIC 256
#define NAME 256
#define EXT 64
#define SHAPE 8
#define FEATURES (HIST + PAIRS + MAGIC + NAME + EXT + SHAPE)

typedef struct layer
{
    unsigned int inputs;
    unsigned int outputs;
    unsigned int relu;
    double in_scale;
    double out_scale;
    float *w_scale;
    int *bias;
    const signed char *weights;
} layer_t;

typedef struct model
{
    unsigned int head_count;
    unsigned int classes[HEADS_MAX];
    char names[HEADS_MAX][CLASSES_MAX][24];
    unsigned int layer_count;
    layer_t layers[LAYERS_MAX];
} model_t;

typedef struct result
{
    int type;
    int source;
    double type_confidence;
    double source_confidence;
} result_t;

typedef struct rule_folder
{
    const char *folder;
    const char *extensions;
} rule_folder_t;

static model_t model;
static double features[FEATURES];
static int xq[WIDTH_MAX * 4];
static int next_xq[WIDTH_MAX * 4];
static double logits[CLASSES_MAX * HEADS_MAX];
static unsigned char head[HEAD_BYTES];
static char files[FILES_MAX][FILE_NAME_MAX];
static char log_text[FILES_MAX * PATH_MAX * 2];

static const char *type_folders[] = {"Images", "Audio", "Videos", "Documents", "Text",
                                     "Code", "Programs", "Archives", "AI Models", "Data"};
static const char *source_folders[] = {"WhatsApp", "Telegram", "Camera", "Screenshots",
                                       "Screen Recordings", ""};

static const rule_folder_t rule_folders[] = {
    {"Images", " jpg jpeg png gif bmp webp heic heif tif tiff svg ico raw cr2 nef "},
    {"Videos", " mp4 mkv webm avi mov m4v 3gp flv wmv mpeg mpg ts "},
    {"Audio", " mp3 wav flac ogg oga opus m4a aac amr wma mid midi "},
    {"Documents", " pdf doc docx odt rtf epub mobi djvu html htm pages "},
    {"Spreadsheets", " xls xlsx ods numbers "},
    {"Presentations", " ppt pptx odp key "},
    {"Text", " txt md rst log tex json xml yaml yml ini conf cfg toml plist env "},
    {"Data", " csv tsv sql db sqlite parquet npy pkl h5 "},
    {"Code", " py sh bash zsh js ts c h cpp hpp java kt go rs rb php cs swift lua pl r ipynb css scss vue jsx tsx "},
    {"Archives", " zip rar 7z tar gz tgz bz2 xz zst lz lzma cab "},
    {"Disk Images", " iso img dmg qcow2 vmdk vdi vhd vhdx "},
    {"Installers", " deb rpm exe msi apk appimage snap flatpak flatpakref pkg run "},
    {"Fonts", " ttf otf woff woff2 "},
    {"AI Models", " gguf safetensors onnx pt pth ckpt tflite knm "},
    {"Torrents", " torrent "},
};

static const char *rule_only[] = {"Spreadsheets", "Presentations", "Disk Images", "Installers", "Fonts", "Torrents"};

static const struct
{
    const char *magic;
    unsigned long length;
    const char *folder;
} signatures[] = {
    {"%PDF", 4, "Documents"}, {"\x89PNG", 4, "Images"}, {"\xff\xd8\xff", 3, "Images"},
    {"GIF8", 4, "Images"}, {"PK\x03\x04", 4, "Archives"}, {"Rar!", 4, "Archives"},
    {"7z\xbc\xaf", 4, "Archives"}, {"\x1f\x8b", 2, "Archives"}, {"BZh", 3, "Archives"},
    {"\xfd" "7zXZ", 5, "Archives"}, {"\x28\xb5\x2f\xfd", 4, "Archives"}, {"OggS", 4, "Audio"},
    {"ID3", 3, "Audio"}, {"fLaC", 4, "Audio"}, {"\x1a\x45\xdf\xa3", 4, "Videos"},
    {"!<arch>\ndebian", 14, "Installers"}, {"\xed\xab\xee\xdb", 4, "Installers"}, {"MZ", 2, "Installers"},
    {"hsqs", 4, "Installers"}, {"\x7f" "ELF", 4, "Programs"}, {"GGUF", 4, "AI Models"},
    {"wOFF", 4, "Fonts"}, {"wOF2", 4, "Fonts"}, {"OTTO", 4, "Fonts"}, {"d8:announce", 11, "Torrents"},
    {"SQLite format 3", 15, "Data"},
};

static unsigned int fnv1a(const unsigned char *data, unsigned long length)
{
    unsigned int value = 0x811C9DC5u;

    for (unsigned long i = 0; i < length; i++)
    {
        value ^= data[i];
        value *= 0x01000193u;
    }

    return value;
}

static double round_even(double value)
{
    long whole = (long)value;

    if (value < 0 && (double)whole != value)
    {
        whole--;
    }

    double fraction = value - (double)whole;

    if (fraction > 0.5 || (fraction == 0.5 && (whole & 1)))
    {
        whole++;
    }

    return (double)whole;
}

static int quantize(double value)
{
    double scaled = round_even(value * 127.0);

    return scaled < 0 ? 0 : scaled > 127 ? 127 : (int)scaled;
}

static double log2_of(double value)
{
    int exponent = 0;

    while (value >= 2.0)
    {
        value /= 2.0;
        exponent++;
    }

    while (value < 1.0)
    {
        value *= 2.0;
        exponent--;
    }

    double t = (value - 1.0) / (value + 1.0);
    double t2 = t * t;
    double sum = 0.0;
    double power = t;

    for (int k = 1; k < 30; k += 2)
    {
        sum += power / k;
        power *= t2;
    }

    return exponent + 2.0 * sum / 0.69314718055994530942;
}

static double exp_of(double value)
{
    if (value < -700)
    {
        return 0.0;
    }

    int halvings = 0;

    while (value < -0.5)
    {
        value /= 2.0;
        halvings++;
    }

    double term = 1.0;
    double sum = 1.0;

    for (int k = 1; k < 20; k++)
    {
        term *= value / k;
        sum += term;
    }

    while (halvings-- > 0)
    {
        sum *= sum;
    }

    return sum;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static void extension_of(const char *name, char *out)
{
    const char *dot = 0;

    for (const char *p = name; *p; p++)
    {
        if (*p == '.')
        {
            dot = p;
        }
    }

    out[0] = 0;

    if (dot == 0)
    {
        return;
    }

    int i = 0;

    for (const char *p = dot + 1; *p && i < 8; p++)
    {
        out[i++] = lower(*p);
    }

    out[i] = 0;
}

static void compute_features(const char *name, unsigned long size, unsigned long length)
{
    unsigned int counts[256];
    unsigned int buckets[PAIRS];

    memset(features, 0, sizeof(features));
    memset(counts, 0, sizeof(counts));
    memset(buckets, 0, sizeof(buckets));

    for (unsigned long i = 0; i < length; i++)
    {
        counts[head[i]]++;
    }

    for (int b = 0; b < 256; b++)
    {
        features[b] = (counts[b] < 32 ? counts[b] : 32) / 32.0;
    }

    for (unsigned long i = 0; i + 1 < length; i++)
    {
        buckets[fnv1a(&head[i], 2) % PAIRS]++;
    }

    for (int i = 0; i < PAIRS; i++)
    {
        features[HIST + i] = (buckets[i] < 16 ? buckets[i] : 16) / 16.0;
    }

    for (unsigned long position = 0; position < 263; position++)
    {
        if (position >= 32 && position < 257)
        {
            continue;
        }

        if (position < length)
        {
            unsigned char key[3] = {(unsigned char)(position & 0xFF), (unsigned char)(position >> 8), head[position]};
            features[HIST + PAIRS + fnv1a(key, 3) % MAGIC] = 1.0;
        }
    }

    unsigned char text[FILE_NAME_MAX + 2];
    unsigned long text_length = 0;

    text[text_length++] = '^';

    for (const char *p = name; *p && text_length < sizeof(text) - 1; p++)
    {
        text[text_length++] = (unsigned char)lower(*p);
    }

    text[text_length++] = '$';

    unsigned int trigrams[NAME];

    memset(trigrams, 0, sizeof(trigrams));

    for (unsigned long i = 0; i + 2 < text_length; i++)
    {
        trigrams[fnv1a(&text[i], 3) % NAME]++;
    }

    for (int i = 0; i < NAME; i++)
    {
        features[HIST + PAIRS + MAGIC + i] = (trigrams[i] < 4 ? trigrams[i] : 4) / 4.0;
    }

    char extension[9];

    extension_of(name, extension);

    if (extension[0])
    {
        features[HIST + PAIRS + MAGIC + NAME + fnv1a((const unsigned char *)extension, strlen(extension)) % EXT] = 1.0;
    }

    double *shape = &features[HIST + PAIRS + MAGIC + NAME + EXT];
    unsigned long letters = strlen(name);
    unsigned long digits = 0;
    unsigned long upper = 0;
    unsigned long spaces = 0;
    unsigned long printable = 0;

    for (const char *p = name; *p; p++)
    {
        digits += *p >= '0' && *p <= '9';
        upper += *p >= 'A' && *p <= 'Z';
        spaces += *p == ' ';
    }

    for (unsigned long i = 0; i < length; i++)
    {
        unsigned char b = head[i];
        printable += (b >= 32 && b < 127) || b == 9 || b == 10 || b == 13 || b >= 0xC2;
    }

    double size_log = log2_of((double)size + 1.0);

    shape[0] = extension[0] ? 1.0 : 0.0;
    shape[1] = (letters < 64 ? letters : 64) / 64.0;
    shape[2] = (double)digits / (letters ? letters : 1);
    shape[3] = (double)upper / (letters ? letters : 1);
    shape[4] = (spaces < 8 ? spaces : 8) / 8.0;
    shape[5] = (size_log < 40 ? size_log : 40) / 40.0;
    shape[6] = length ? (double)printable / length : 0.0;
    shape[7] = (double)length / HEAD_BYTES;
}

static void infer(result_t *result)
{
    unsigned int width = FEATURES;

    for (unsigned int i = 0; i < FEATURES; i++)
    {
        xq[i] = quantize(features[i]);
    }

    for (unsigned int l = 0; l < model.layer_count; l++)
    {
        layer_t *layer = &model.layers[l];

        for (unsigned int o = 0; o < layer->outputs; o++)
        {
            const signed char *row = layer->weights + (unsigned long)o * layer->inputs;
            long acc = layer->bias[o];

            for (unsigned int i = 0; i < width; i++)
            {
                acc += (long)row[i] * xq[i];
            }

            double real = (double)acc * (layer->in_scale * (double)layer->w_scale[o]);

            if (layer->relu)
            {
                double scaled = round_even((real > 0 ? real : 0) / layer->out_scale);
                next_xq[o] = scaled > 127 ? 127 : (int)scaled;
            }
            else
            {
                logits[o] = real;
            }
        }

        if (layer->relu)
        {
            memcpy(xq, next_xq, layer->outputs * sizeof(int));
        }

        width = layer->outputs;
    }

    unsigned int start = 0;

    for (unsigned int h = 0; h < model.head_count; h++)
    {
        double highest = logits[start];
        int best = 0;

        for (unsigned int c = 1; c < model.classes[h]; c++)
        {
            if (logits[start + c] > highest)
            {
                highest = logits[start + c];
                best = (int)c;
            }
        }

        double total = 0.0;

        for (unsigned int c = 0; c < model.classes[h]; c++)
        {
            total += exp_of(logits[start + c] - highest);
        }

        if (h == 0)
        {
            result->type = best;
            result->type_confidence = 1.0 / total;
        }
        else
        {
            result->source = best;
            result->source_confidence = 1.0 / total;
        }

        start += model.classes[h];
    }
}

static unsigned int read_u32(const unsigned char **p)
{
    unsigned int value;

    memcpy(&value, *p, 4);
    *p += 4;
    return value;
}

static float read_f32(const unsigned char **p)
{
    float value;

    memcpy(&value, *p, 4);
    *p += 4;
    return value;
}

static int load_model(void)
{
    file_stat_t info;

    if (stat(MODEL_PATH, &info) != 0)
    {
        return -1;
    }

    unsigned char *data = mem_alloc(info.size);
    int fd = open(MODEL_PATH, O_READ);

    if (data == 0 || fd < 0 || read(fd, data, info.size) != (long)info.size)
    {
        return -1;
    }

    close(fd);

    if (memcmp_bytes(data, "KNOCNN01", 8) != 0)
    {
        return -1;
    }

    const unsigned char *p = data + 8;

    read_u32(&p);

    unsigned int config[6];

    for (int i = 0; i < 6; i++)
    {
        config[i] = read_u32(&p);
    }

    if (config[0] != HIST || config[1] != PAIRS || config[2] != MAGIC ||
        config[3] != NAME || config[4] != EXT || config[5] != SHAPE)
    {
        return -1;
    }

    model.head_count = read_u32(&p);

    if (model.head_count != HEADS_MAX)
    {
        return -1;
    }

    for (unsigned int h = 0; h < model.head_count; h++)
    {
        model.classes[h] = read_u32(&p);

        if (model.classes[h] > CLASSES_MAX)
        {
            return -1;
        }

        for (unsigned int c = 0; c < model.classes[h]; c++)
        {
            unsigned int length = *p++;

            for (unsigned int i = 0; i < length && i < 23; i++)
            {
                model.names[h][c][i] = (char)p[i];
            }

            model.names[h][c][length < 23 ? length : 23] = 0;
            p += length;
        }
    }

    model.layer_count = read_u32(&p);

    if (model.layer_count > LAYERS_MAX)
    {
        return -1;
    }

    for (unsigned int l = 0; l < model.layer_count; l++)
    {
        layer_t *layer = &model.layers[l];

        layer->inputs = read_u32(&p);
        layer->outputs = read_u32(&p);
        layer->relu = read_u32(&p);
        layer->in_scale = read_f32(&p);
        layer->out_scale = read_f32(&p);

        if (layer->outputs > WIDTH_MAX * 4 || layer->inputs > FEATURES)
        {
            return -1;
        }

        layer->w_scale = mem_alloc(layer->outputs * sizeof(float));
        layer->bias = mem_alloc(layer->outputs * sizeof(int));

        for (unsigned int o = 0; o < layer->outputs; o++)
        {
            layer->w_scale[o] = read_f32(&p);
        }

        for (unsigned int o = 0; o < layer->outputs; o++)
        {
            layer->bias[o] = (int)read_u32(&p);
        }

        layer->weights = (const signed char *)p;
        p += (unsigned long)layer->outputs * layer->inputs;
    }

    return p <= data + info.size ? 0 : -1;
}

static int starts_with_ci(const char *text, const char *prefix)
{
    while (*prefix)
    {
        if (lower(*text++) != lower(*prefix++))
        {
            return 0;
        }
    }

    return 1;
}

static int digits_at(const char *text, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            return 0;
        }
    }

    return 1;
}

static const char *name_source(const char *name)
{
    static const char *whatsapp_prefixes[] = {"IMG-", "VID-", "AUD-", "PTT-", "DOC-", "STK-"};
    static const char *whatsapp_web[] = {"WhatsApp Image", "WhatsApp Video", "WhatsApp Audio",
                                         "WhatsApp Ptt", "WhatsApp Document"};
    static const char *telegram[] = {"photo_", "video_", "file_", "audio_"};
    static const char *recordings[] = {"Screen Recording", "Screencast", "Screenrecorder", "screen-recording"};
    static const char *camera[] = {"IMG_", "VID_", "PXL_", "DSC_", "MVIMG_"};

    for (int i = 0; i < 6; i++)
    {
        if (starts_with_ci(name, whatsapp_prefixes[i]) && digits_at(name + 4, 8) &&
            starts_with_ci(name + 12, "-WA") && digits_at(name + 15, 4))
        {
            return "WhatsApp";
        }
    }

    for (int i = 0; i < 5; i++)
    {
        if (starts_with_ci(name, whatsapp_web[i]))
        {
            return "WhatsApp";
        }
    }

    for (int i = 0; i < 4; i++)
    {
        unsigned long length = strlen(telegram[i]);

        if (starts_with_ci(name, telegram[i]) && digits_at(name + length, 1))
        {
            const char *p = name + length;

            while (*p >= '0' && *p <= '9')
            {
                p++;
            }

            if (*p == '_' || *p == '@' || *p == '-')
            {
                return "Telegram";
            }
        }
    }

    if (starts_with_ci(name, "Screenshot") || starts_with_ci(name, "Screen Shot"))
    {
        return "Screenshots";
    }

    for (int i = 0; i < 4; i++)
    {
        if (starts_with_ci(name, recordings[i]))
        {
            return "Screen Recordings";
        }
    }

    for (int i = 0; i < 5; i++)
    {
        unsigned long length = strlen(camera[i]);

        if (starts_with_ci(name, camera[i]) && digits_at(name + length, 4))
        {
            return "Camera";
        }
    }

    return 0;
}

static const char *extension_folder(const char *name)
{
    char extension[12];
    char key[14];
    const char *lowered = name;
    unsigned long length = strlen(name);

    if (length > 7 && (starts_with_ci(name + length - 7, ".tar.gz") || starts_with_ci(name + length - 7, ".tar.xz")))
    {
        lowered = "x.gz";
    }

    extension_of(lowered, extension);

    if (!extension[0])
    {
        return 0;
    }

    key[0] = ' ';
    strcpy(key + 1, extension);
    length = strlen(key);
    key[length] = ' ';
    key[length + 1] = 0;

    for (unsigned long i = 0; i < sizeof(rule_folders) / sizeof(rule_folders[0]); i++)
    {
        const char *list = rule_folders[i].extensions;

        for (const char *p = list; *p; p++)
        {
            if (starts_with_ci(p, key))
            {
                return rule_folders[i].folder;
            }
        }
    }

    return 0;
}

static const char *signature_folder(const char *path, unsigned long length)
{
    for (unsigned long i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++)
    {
        if (length >= signatures[i].length &&
            memcmp_bytes(head, signatures[i].magic, signatures[i].length) == 0)
        {
            return signatures[i].folder;
        }
    }

    if (length >= 12 && memcmp_bytes(head + 4, "ftyp", 4) == 0)
    {
        return "Videos";
    }

    unsigned char iso[5];
    int fd = open(path, O_READ);

    if (fd >= 0)
    {
        seek(fd, 32769);
        long count = read(fd, iso, 5);
        close(fd);

        if (count == 5 && memcmp_bytes(iso, "CD001", 5) == 0)
        {
            return "Disk Images";
        }
    }

    if (length == 0)
    {
        return 0;
    }

    for (unsigned long i = 0; i < length; i++)
    {
        unsigned char b = head[i];

        if (!((b >= 32 && b < 127) || b == 9 || b == 10 || b == 13))
        {
            return 0;
        }
    }

    return "Text";
}

static void join(char *out, const char *a, const char *b)
{
    strcpy(out, a);

    unsigned long length = strlen(out);

    if (b[0])
    {
        if (length > 0 && out[length - 1] != '/')
        {
            out[length++] = '/';
        }

        strcpy(out + length, b);
    }
}

static int rule_for(const char *path, const char *name, unsigned long length, char *out)
{
    const char *folder = extension_folder(name);

    if (folder == 0)
    {
        folder = signature_folder(path, length);
    }

    if (folder == 0)
    {
        return 0;
    }

    const char *source = name_source(name);

    if (source == 0)
    {
        strcpy(out, folder);
    }
    else if (strcmp(source, "Screenshots") == 0 || strcmp(source, "Screen Recordings") == 0)
    {
        strcpy(out, source);
    }
    else if (strcmp(source, "Camera") == 0 && strcmp(folder, "Images") == 0)
    {
        strcpy(out, "Camera/Photos");
    }
    else
    {
        join(out, source, folder);
    }

    return 1;
}

static int is_rule_only(const char *folder)
{
    const char *last = folder;

    for (const char *p = folder; *p; p++)
    {
        if (*p == '/')
        {
            last = p + 1;
        }
    }

    for (unsigned long i = 0; i < sizeof(rule_only) / sizeof(rule_only[0]); i++)
    {
        if (strcmp(last, rule_only[i]) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static void ai_folder(const result_t *result, char *out)
{
    const char *type = type_folders[result->type];
    const char *source = source_folders[result->source];

    if (!source[0])
    {
        strcpy(out, type);
    }
    else if (result->source == 3 || result->source == 4)
    {
        strcpy(out, source);
    }
    else if (result->source == 2 && result->type == 0)
    {
        strcpy(out, "Camera/Photos");
    }
    else if (result->source == 0 && result->type == 1)
    {
        strcpy(out, "WhatsApp/Voice Notes");
    }
    else
    {
        join(out, source, type);
    }
}

static const char *choose(const char *path, const char *name, unsigned long length,
                          const result_t *result, char *folder)
{
    char rule[PATH_MAX];
    int has_rule = rule_for(path, name, length, rule);

    if (has_rule && is_rule_only(rule))
    {
        strcpy(folder, rule);
        return "rules";
    }

    double confidence = result->type_confidence < result->source_confidence
                            ? result->type_confidence
                            : result->source_confidence;

    if (confidence >= MIN_CONFIDENCE)
    {
        ai_folder(result, folder);
        return "ai";
    }

    if (has_rule)
    {
        strcpy(folder, rule);
        return "rules";
    }

    strcpy(folder, "Random");
    return "random";
}

static void make_folders(const char *path)
{
    char partial[PATH_MAX];
    unsigned long length = strlen(path);

    for (unsigned long i = 1; i <= length; i++)
    {
        if (path[i] == '/' || path[i] == 0)
        {
            memcpy(partial, path, i);
            partial[i] = 0;
            mkdir(partial);
        }
    }
}

static void free_target(char *target)
{
    file_stat_t info;

    if (stat(target, &info) != 0)
    {
        return;
    }

    char base[PATH_MAX];
    char extension[PATH_MAX];
    unsigned long length = strlen(target);
    unsigned long dot = length;

    for (unsigned long i = length; i > 0; i--)
    {
        if (target[i - 1] == '.')
        {
            dot = i - 1;
            break;
        }

        if (target[i - 1] == '/')
        {
            break;
        }
    }

    memcpy(base, target, dot);
    base[dot] = 0;
    strcpy(extension, target + dot);

    for (unsigned long n = 1; n < 1000; n++)
    {
        char number[24];
        unsigned long digits = 0;
        unsigned long value = n;

        do
        {
            number[digits++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);

        char *p = target;
        strcpy(p, base);
        p += strlen(p);
        *p++ = ' ';
        *p++ = '(';

        while (digits)
        {
            *p++ = number[--digits];
        }

        *p++ = ')';
        strcpy(p, extension);

        if (stat(target, &info) != 0)
        {
            return;
        }
    }
}

static void print_percent(double value)
{
    unsigned long percent = (unsigned long)round_even(value * 100);

    print(percent < 10 ? "  " : percent < 100 ? " " : "");
    print_uint(percent);
    print("%");
}

static void organize(const char *directory, int apply)
{
    dir_entry_t entry;
    unsigned long count = 0;
    unsigned long layers[3] = {0, 0, 0};

    for (unsigned long i = 0; readdir(directory, i, &entry) == 0 && count < FILES_MAX; i++)
    {
        if (entry.type == FILE_TYPE_FILE && entry.name[0] != '.')
        {
            strcpy(files[count++], entry.name);
        }
    }

    if (load_model() != 0)
    {
        print("organize: cannot load " MODEL_PATH "\n");
        exit(1);
    }

    char *log_end = log_text;

    print("file                                     AI type          AI source             layer   destination\n");

    for (unsigned long f = 0; f < count; f++)
    {
        char path[PATH_MAX];
        char folder[PATH_MAX];
        char target[PATH_MAX];
        file_stat_t info;
        result_t result;

        join(path, directory, files[f]);

        if (stat(path, &info) != 0)
        {
            continue;
        }

        int fd = open(path, O_READ);
        long length = fd >= 0 ? read(fd, head, HEAD_BYTES) : 0;

        if (fd >= 0)
        {
            close(fd);
        }

        if (length < 0)
        {
            length = 0;
        }

        compute_features(files[f], info.size, (unsigned long)length);
        infer(&result);

        const char *layer = choose(path, files[f], (unsigned long)length, &result, folder);

        layers[layer[0] == 'a' ? 0 : layer[0] == 'r' && layer[1] == 'u' ? 1 : 2]++;

        char folder_path[PATH_MAX];

        join(folder_path, directory, folder);
        join(target, folder_path, files[f]);
        free_target(target);

        char shown[42];
        unsigned long n = 0;

        while (files[f][n] && n < 40)
        {
            shown[n] = files[f][n];
            n++;
        }

        shown[n] = 0;
        print_padded(shown, 41);
        print_padded(model.names[0][result.type], 9);
        print_percent(result.type_confidence);
        print("     ");
        print_padded(model.names[1][result.source], 17);
        print_percent(result.source_confidence);
        print("  ");
        print_padded(layer, 8);
        print(target + strlen(directory) + 1);
        print("\n");

        if (apply)
        {
            make_folders(folder_path);

            if (rename(path, target) == 0)
            {
                strcpy(log_end, path);
                log_end += strlen(log_end);
                *log_end++ = '\t';
                strcpy(log_end, target);
                log_end += strlen(log_end);
                *log_end++ = '\n';
                *log_end = 0;
            }
            else
            {
                print("  could not move this file\n");
            }
        }
    }

    print("\n");
    print_uint(count);
    print(" files: ");
    print_uint(layers[0]);
    print(" by the AI, ");
    print_uint(layers[1]);
    print(" by the rules, ");
    print_uint(layers[2]);
    print(" to Random/\n");

    if (!apply)
    {
        print("Nothing moved (plan only). Run: organize ");
        print(directory);
        print(" --apply\n");
        return;
    }

    char log_path[PATH_MAX];

    join(log_path, directory, LOG_NAME);

    int fd = open(log_path, O_WRITE | O_CREATE | O_TRUNC);

    if (fd >= 0)
    {
        write(fd, log_text, (unsigned long)(log_end - log_text));
        close(fd);
    }

    print("Files moved. Undo with: organize ");
    print(directory);
    print(" --undo\n");
}

static void remove_empty_parents(const char *path, const char *directory)
{
    char folder[PATH_MAX];

    strcpy(folder, path);

    while (1)
    {
        unsigned long length = strlen(folder);

        while (length > 0 && folder[length - 1] != '/')
        {
            length--;
        }

        if (length <= 1)
        {
            return;
        }

        folder[length - 1] = 0;

        if (strcmp(folder, directory) == 0 || remove(folder) != 0)
        {
            return;
        }
    }
}

static void undo(const char *directory)
{
    char log_path[PATH_MAX];

    join(log_path, directory, LOG_NAME);

    int fd = open(log_path, O_READ);

    if (fd < 0)
    {
        print("organize: nothing to undo in ");
        print(directory);
        print("\n");
        return;
    }

    long length = read(fd, log_text, sizeof(log_text) - 1);

    close(fd);
    log_text[length > 0 ? length : 0] = 0;

    char *lines[FILES_MAX];
    int count = 0;

    for (char *p = log_text; *p && count < FILES_MAX;)
    {
        lines[count++] = p;

        while (*p && *p != '\n')
        {
            p++;
        }

        if (*p)
        {
            *p++ = 0;
        }
    }

    int restored = 0;

    for (int i = count - 1; i >= 0; i--)
    {
        char *tab = lines[i];

        while (*tab && *tab != '\t')
        {
            tab++;
        }

        if (!*tab)
        {
            continue;
        }

        *tab = 0;

        const char *from = lines[i];
        const char *to = tab + 1;

        if (rename(to, from) == 0)
        {
            restored++;
            remove_empty_parents(to, directory);
        }
    }

    remove(log_path);
    print("Restored ");
    print_uint((unsigned long)restored);
    print(" files and removed the empty folders\n");
}

int main(void)
{
    char args[ARGS_MAX];
    char *words[4];
    int count = 0;

    getargs(args, sizeof(args));

    for (char *p = args; *p && count < 4;)
    {
        while (*p == ' ')
        {
            *p++ = 0;
        }

        if (!*p)
        {
            break;
        }

        words[count++] = p;

        while (*p && *p != ' ')
        {
            p++;
        }
    }

    const char *directory = DEFAULT_DIR;
    int apply = 0;
    int reverse = 0;

    for (int i = 0; i < count; i++)
    {
        if (strcmp(words[i], "--apply") == 0)
        {
            apply = 1;
        }
        else if (strcmp(words[i], "--undo") == 0)
        {
            reverse = 1;
        }
        else
        {
            directory = words[i];
        }
    }

    file_stat_t info;

    if (stat(directory, &info) != 0 || info.type != FILE_TYPE_DIR)
    {
        print("usage: organize [DIR] [--apply | --undo]   (default DIR: " DEFAULT_DIR ")\n");
        return 1;
    }

    if (reverse)
    {
        undo(directory);
    }
    else
    {
        organize(directory, apply);
    }

    return 0;
}
