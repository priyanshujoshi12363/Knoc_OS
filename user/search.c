#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "search.h"
#include "ulib.h"

#define MAGIC "KNIDX001"
#define CONTENT_BYTES 3000
#define DEPTH_MAX 8

static const char *type_names[TYPE_COUNT] = {"other", "text", "document", "image", "audio",
                                             "video", "code", "archive", "data"};

static const struct
{
    const char *extension;
    unsigned int type;
} extensions[] = {
    {"txt", TYPE_TEXT},       {"md", TYPE_TEXT},        {"log", TYPE_TEXT},       {"pdf", TYPE_DOCUMENT},
    {"doc", TYPE_DOCUMENT},   {"docx", TYPE_DOCUMENT},  {"odt", TYPE_DOCUMENT},   {"rtf", TYPE_DOCUMENT},
    {"ppt", TYPE_DOCUMENT},   {"pptx", TYPE_DOCUMENT},  {"xls", TYPE_DOCUMENT},   {"xlsx", TYPE_DOCUMENT},
    {"jpg", TYPE_IMAGE},      {"jpeg", TYPE_IMAGE},     {"png", TYPE_IMAGE},      {"gif", TYPE_IMAGE},
    {"bmp", TYPE_IMAGE},      {"webp", TYPE_IMAGE},     {"heic", TYPE_IMAGE},     {"svg", TYPE_IMAGE},
    {"mp3", TYPE_AUDIO},      {"wav", TYPE_AUDIO},      {"flac", TYPE_AUDIO},     {"ogg", TYPE_AUDIO},
    {"m4a", TYPE_AUDIO},      {"mp4", TYPE_VIDEO},      {"mkv", TYPE_VIDEO},      {"avi", TYPE_VIDEO},
    {"mov", TYPE_VIDEO},      {"webm", TYPE_VIDEO},     {"c", TYPE_CODE},         {"h", TYPE_CODE},
    {"py", TYPE_CODE},        {"js", TYPE_CODE},        {"ts", TYPE_CODE},        {"java", TYPE_CODE},
    {"cpp", TYPE_CODE},       {"rs", TYPE_CODE},        {"go", TYPE_CODE},        {"sh", TYPE_CODE},
    {"ksh", TYPE_CODE},       {"rb", TYPE_CODE},        {"php", TYPE_CODE},       {"html", TYPE_CODE},
    {"css", TYPE_CODE},       {"s", TYPE_CODE},         {"zip", TYPE_ARCHIVE},    {"tar", TYPE_ARCHIVE},
    {"gz", TYPE_ARCHIVE},     {"bz2", TYPE_ARCHIVE},    {"xz", TYPE_ARCHIVE},     {"7z", TYPE_ARCHIVE},
    {"rar", TYPE_ARCHIVE},    {"deb", TYPE_ARCHIVE},    {"iso", TYPE_ARCHIVE},    {"json", TYPE_DATA},
    {"csv", TYPE_DATA},       {"xml", TYPE_DATA},       {"yaml", TYPE_DATA},      {"yml", TYPE_DATA},
    {"toml", TYPE_DATA},      {"ini", TYPE_DATA},
};

static char text[CONTENT_BYTES + 2 * INDEX_PATH_MAX + 64];

static const char *type_words[TYPE_COUNT] = {"", "text", "document", "image picture photo", "audio music song",
                                             "video movie", "source code program", "archive", "data"};
static unsigned char content[CONTENT_BYTES];

const char *search_type_name(unsigned int type)
{
    return type < TYPE_COUNT ? type_names[type] : "other";
}

unsigned int search_type_of(const char *name)
{
    const char *dot = strrchr(name, '.');

    if (!dot || !dot[1])
    {
        return TYPE_OTHER;
    }

    for (unsigned long i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
    {
        if (strcasecmp(dot + 1, extensions[i].extension) == 0)
        {
            return extensions[i].type;
        }
    }

    return TYPE_OTHER;
}

int search_load(search_index_t *index)
{
    FILE *f = fopen(INDEX_FILE, "r");
    char magic[8];
    unsigned int count = 0;

    index->count = 0;
    index->updated = 0;

    if (!f)
    {
        return -1;
    }

    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) != 0 || fread(&count, 4, 1, f) != 1 ||
        fread(&index->updated, 8, 1, f) != 1 || count > INDEX_FILES_MAX)
    {
        fclose(f);
        return -1;
    }

    if ((int)count > index->room)
    {
        index_entry_t *grown = realloc(index->entries, count * sizeof(index_entry_t));

        if (!grown)
        {
            fclose(f);
            return -1;
        }

        index->entries = grown;
        index->room = (int)count;
    }

    index->count = (int)fread(index->entries, sizeof(index_entry_t), count, f);
    fclose(f);
    return index->count == (int)count ? 0 : -1;
}

int search_save(const search_index_t *index)
{
    unsigned int count = (unsigned int)index->count;

    mkdir("/var");
    mkdir(INDEX_DIR);

    FILE *f = fopen(INDEX_FILE ".new", "w");

    if (!f)
    {
        return -1;
    }

    fwrite(MAGIC, 1, 8, f);
    fwrite(&count, 4, 1, f);
    fwrite(&index->updated, 8, 1, f);

    int ok = fwrite(index->entries, sizeof(index_entry_t), count, f) == count;

    fclose(f);

    if (!ok)
    {
        remove(INDEX_FILE ".new");
        return -1;
    }

    remove(INDEX_FILE);
    return rename(INDEX_FILE ".new", INDEX_FILE);
}

void search_vector(const index_entry_t *entry, float *out)
{
    for (int i = 0; i < INDEX_VECTOR; i++)
    {
        out[i] = entry->vector[i] * entry->scale;
    }
}

static int is_text(const unsigned char *data, long length)
{
    long odd = 0;

    for (long i = 0; i < length; i++)
    {
        unsigned char c = data[i];

        odd += c == 0 || (c < 32 && c != '\n' && c != '\r' && c != '\t');
    }

    return length > 0 && odd * 50 <= length;
}

static void append_words(size_t *used, const char *from, size_t length)
{
    for (size_t i = 0; i < length && *used < sizeof(text) - 2; i++)
    {
        char c = from[i];

        text[(*used)++] = isalnum((unsigned char)c) || (unsigned char)c >= 128 ? c : ' ';
    }

    text[(*used)++] = ' ';
}

static void describe(index_entry_t *entry, const embed_model_t *model)
{
    const char *name = strrchr(entry->path, '/') ? strrchr(entry->path, '/') + 1 : entry->path;
    const char *folders = entry->path + strlen(INDEX_ROOT);
    size_t folder_length = (size_t)(name - folders);
    size_t used = 0;
    long got = 0;

    entry->type = search_type_of(name);
    entry->snippet[0] = 0;

    FILE *f = fopen(entry->path, "r");

    if (f)
    {
        got = (long)fread(content, 1, sizeof(content), f);
        fclose(f);
    }

    append_words(&used, name, strlen(name));
    append_words(&used, name, strlen(name));
    append_words(&used, folders, folder_length);

    append_words(&used, type_words[entry->type], strlen(type_words[entry->type]));

    if (got > 0 && is_text(content, got) && entry->type != TYPE_DOCUMENT && entry->type != TYPE_IMAGE)
    {
        size_t s = 0;
        int space = 0;

        if (entry->type == TYPE_OTHER)
        {
            entry->type = TYPE_TEXT;
        }

        for (long i = 0; i < got && s < INDEX_SNIPPET - 1; i++)
        {
            unsigned char c = content[i];

            if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
            {
                space = s > 0;
                continue;
            }

            if (space && s < INDEX_SNIPPET - 2)
            {
                entry->snippet[s++] = ' ';
            }

            space = 0;
            entry->snippet[s++] = (char)c;
        }

        entry->snippet[s] = 0;

        size_t room = sizeof(text) - used - 1;
        size_t take = (size_t)got < room ? (size_t)got : room;

        memcpy(text + used, content, take);
        used += take;
    }

    text[used] = 0;

    float vector[EMBED_DIM_MAX];
    float largest = 0;

    embed_text(model, text, used, vector);

    for (int i = 0; i < INDEX_VECTOR; i++)
    {
        float v = i < (int)model->dim ? vector[i] : 0;

        largest = v > largest ? v : -v > largest ? -v : largest;
    }

    entry->scale = largest > 0 ? largest / 127.0f : 1.0f;

    for (int i = 0; i < INDEX_VECTOR; i++)
    {
        float v = i < (int)model->dim ? vector[i] / entry->scale : 0;

        entry->vector[i] = (signed char)(v > 0 ? v + 0.5f : v - 0.5f);
    }
}

static index_entry_t *find_entry(search_index_t *index, const char *path)
{
    for (int i = 0; i < index->count; i++)
    {
        if (strcmp(index->entries[i].path, path) == 0)
        {
            return &index->entries[i];
        }
    }

    return 0;
}

static index_entry_t *new_entry(search_index_t *index)
{
    if (index->count >= INDEX_FILES_MAX)
    {
        return 0;
    }

    if (index->count == index->room)
    {
        int room = index->room ? index->room * 2 : 256;
        index_entry_t *grown = realloc(index->entries, (size_t)room * sizeof(index_entry_t));

        if (!grown)
        {
            return 0;
        }

        index->entries = grown;
        index->room = room;
    }

    index_entry_t *entry = &index->entries[index->count++];

    memset(entry, 0, sizeof(*entry));
    return entry;
}

static void walk(search_index_t *index, const embed_model_t *model, const char *folder, int depth, int *added)
{
    dir_entry_t item;
    char path[INDEX_PATH_MAX];

    for (unsigned long i = 0; readdir(folder, i, &item) == 0; i++)
    {
        if (item.name[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", folder, item.name) >= (int)sizeof(path))
        {
            continue;
        }

        if (item.type == FILE_TYPE_DIR)
        {
            if (depth < DEPTH_MAX)
            {
                walk(index, model, path, depth + 1, added);
            }

            continue;
        }

        index_entry_t *entry = find_entry(index, path);

        if (entry && entry->modified == item.modified && entry->size == item.size)
        {
            entry->seen = 1;
            continue;
        }

        if (!entry)
        {
            entry = new_entry(index);

            if (!entry)
            {
                continue;
            }

            snprintf(entry->path, sizeof(entry->path), "%s", path);
        }

        entry->modified = item.modified;
        entry->size = item.size;
        entry->seen = 1;
        describe(entry, model);
        (*added)++;
    }
}

int search_refresh(search_index_t *index, const embed_model_t *model, int *added, int *removed)
{
    int kept = 0;

    *added = 0;
    *removed = 0;

    for (int i = 0; i < index->count; i++)
    {
        index->entries[i].seen = 0;
    }

    walk(index, model, INDEX_ROOT, 0, added);

    for (int i = 0; i < index->count; i++)
    {
        if (index->entries[i].seen)
        {
            index->entries[kept++] = index->entries[i];
        }
    }

    *removed = index->count - kept;
    index->count = kept;

    if (*added || *removed)
    {
        long now = realtime();

        index->updated = now > 0 ? (unsigned long)now : 0;
    }

    return *added || *removed;
}
