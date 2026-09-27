#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "search.h"
#include "ulib.h"

#define RESULTS 5
#define MIN_SCORE 0.30f
#define BEST_GAP 0.15f
#define NAME_BONUS 0.30f
#define QUERY_WORDS 32
#define DAY 86400L

typedef struct result
{
    int index;
    float score;
    float meaning;
    float bonus;
} result_t;

static const char *months[] = {"january", "february", "march", "april", "may", "june", "july",
                               "august", "september", "october", "november", "december"};
static const char *short_months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
static const char *stop_words[] = {"the", "a", "an", "of", "for", "from", "in", "on", "my", "me", "to", "and",
                                   "with", "about", "all", "some", "any", "find", "show", "where", "is", "are",
                                   "file", "files", "that", "which", "i", "was", "were", "last", "this", "at"};

static const struct
{
    const char *word;
    const char *meaning;
} aliases[] = {
    {"cv", "cv resume curriculum vitae"},  {"bio", "biography profile"},       {"id", "identity card"},
    {"pic", "picture photo"},              {"pics", "pictures photos"},        {"doc", "document"},
    {"docs", "documents"},                 {"ppt", "presentation slides"},     {"xls", "spreadsheet"},
    {"msg", "message"},                    {"pwd", "password"},                {"addr", "address"},
    {"txn", "transaction"},                {"acct", "account"},                {"repo", "repository source code"},
    {"src", "source code"},                {"config", "configuration settings"},
};

static const char *alias_of(const char *word)
{
    for (unsigned long i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
    {
        if (strcmp(word, aliases[i].word) == 0)
        {
            return aliases[i].meaning;
        }
    }

    return word;
}

static embed_model_t model;
static search_index_t index_data;
static result_t results[INDEX_FILES_MAX];

static long days_from_civil(long y, long m, long d)
{
    y -= m <= 2;

    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, long *m, long *d)
{
    z += 719468;

    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;

    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static long month_start(long y, long m)
{
    while (m < 1)
    {
        m += 12;
        y--;
    }

    while (m > 12)
    {
        m -= 12;
        y++;
    }

    return days_from_civil(y, m, 1) * DAY;
}

static int is_stop(const char *word)
{
    for (unsigned long i = 0; i < sizeof(stop_words) / sizeof(stop_words[0]); i++)
    {
        if (strcmp(word, stop_words[i]) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int month_number(const char *word)
{
    for (int i = 0; i < 12; i++)
    {
        if (strcmp(word, months[i]) == 0 || (strcmp(word, short_months[i]) == 0 && strcmp(word, "may") != 0))
        {
            return i + 1;
        }
    }

    return 0;
}

static unsigned int type_word(const char *w)
{
    static const struct
    {
        const char *word;
        unsigned int type;
    } words[] = {
        {"photo", TYPE_IMAGE},    {"photos", TYPE_IMAGE},    {"picture", TYPE_IMAGE},  {"pictures", TYPE_IMAGE},
        {"image", TYPE_IMAGE},    {"images", TYPE_IMAGE},    {"pic", TYPE_IMAGE},      {"pics", TYPE_IMAGE},
        {"screenshot", TYPE_IMAGE}, {"screenshots", TYPE_IMAGE}, {"pdf", TYPE_DOCUMENT}, {"pdfs", TYPE_DOCUMENT},
        {"document", TYPE_DOCUMENT}, {"documents", TYPE_DOCUMENT}, {"video", TYPE_VIDEO}, {"videos", TYPE_VIDEO},
        {"movie", TYPE_VIDEO},    {"movies", TYPE_VIDEO},    {"song", TYPE_AUDIO},     {"songs", TYPE_AUDIO},
        {"music", TYPE_AUDIO},    {"audio", TYPE_AUDIO},     {"code", TYPE_CODE},      {"archive", TYPE_ARCHIVE},
        {"archives", TYPE_ARCHIVE},
    };

    for (unsigned long i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    {
        if (strcmp(w, words[i].word) == 0)
        {
            return words[i].type;
        }
    }

    return TYPE_OTHER;
}

typedef struct filter
{
    long from;
    long to;
    unsigned int type;
    char label[64];
} filter_t;

static int date_phrase(char words[][32], int count, int i, long now, filter_t *f)
{
    long day = now / DAY;
    long y;
    long m;
    long d;
    const char *w = words[i];
    const char *next = i + 1 < count ? words[i + 1] : "";

    civil_from_days(day, &y, &m, &d);

    if (strcmp(w, "today") == 0)
    {
        f->from = day * DAY;
        f->to = now + DAY;
        snprintf(f->label, sizeof(f->label), "today");
        return 1;
    }

    if (strcmp(w, "yesterday") == 0)
    {
        f->from = (day - 1) * DAY;
        f->to = day * DAY;
        snprintf(f->label, sizeof(f->label), "yesterday");
        return 1;
    }

    if (strcmp(w, "recent") == 0 || strcmp(w, "recently") == 0 || strcmp(w, "latest") == 0)
    {
        f->from = now - 7 * DAY;
        f->to = now + DAY;
        snprintf(f->label, sizeof(f->label), "the last 7 days");
        return 1;
    }

    if ((strcmp(w, "this") == 0 || strcmp(w, "last") == 0 || strcmp(w, "past") == 0) && next[0])
    {
        int last = strcmp(w, "this") != 0;
        long weekday = (day + 3) % 7;

        if (strcmp(next, "week") == 0)
        {
            long start = (day - weekday) * DAY;

            f->from = last ? start - 7 * DAY : start;
            f->to = last ? start : now + DAY;
        }
        else if (strcmp(next, "month") == 0)
        {
            f->from = month_start(y, last ? m - 1 : m);
            f->to = last ? month_start(y, m) : now + DAY;
        }
        else if (strcmp(next, "year") == 0)
        {
            f->from = days_from_civil(last ? y - 1 : y, 1, 1) * DAY;
            f->to = last ? days_from_civil(y, 1, 1) * DAY : now + DAY;
        }
        else
        {
            return 0;
        }

        snprintf(f->label, sizeof(f->label), "%s %s", w, next);
        return 2;
    }

    int month = month_number(w);

    if (month)
    {
        long year = month <= m ? y : y - 1;

        if (i + 1 < count && strlen(next) == 4 && isdigit((unsigned char)next[0]))
        {
            year = atol(next);
        }

        f->from = month_start(year, month);
        f->to = month_start(year, month + 1);
        snprintf(f->label, sizeof(f->label), "%s %ld", months[month - 1], year);
        return year == atol(next) ? 2 : 1;
    }

    if (strlen(w) == 4 && isdigit((unsigned char)w[0]) && atol(w) >= 1990 && atol(w) <= 2100)
    {
        f->from = days_from_civil(atol(w), 1, 1) * DAY;
        f->to = days_from_civil(atol(w) + 1, 1, 1) * DAY;
        snprintf(f->label, sizeof(f->label), "%s", w);
        return 1;
    }

    return 0;
}

static void print_date(unsigned long seconds)
{
    long y;
    long m;
    long d;

    if (!seconds)
    {
        printf("-");
        return;
    }

    civil_from_days((long)(seconds / DAY), &y, &m, &d);
    printf("%04ld-%02ld-%02ld", y, m, d);
}

static float name_bonus(const index_entry_t *entry, char query[][32], int count)
{
    char words[64][32];
    int n = embed_words(entry->path, strlen(entry->path), words, 64);
    int wanted = 0;
    int found = 0;

    for (int q = 0; q < count; q++)
    {
        if (strlen(query[q]) < 3 || is_stop(query[q]))
        {
            continue;
        }

        wanted++;

        for (int i = 0; i < n; i++)
        {
            if (strcmp(words[i], query[q]) == 0 ||
                (strlen(query[q]) >= 4 && strncmp(words[i], query[q], strlen(query[q])) == 0))
            {
                found++;
                break;
            }
        }
    }

    return wanted ? NAME_BONUS * (float)found / (float)wanted : 0;
}

int main(int argc, char **argv)
{
    int verbose = 0;
    int first = 1;
    char query[512] = "";

    if (first < argc && strcmp(argv[first], "-v") == 0)
    {
        verbose = 1;
        first++;
    }

    for (int i = first; i < argc; i++)
    {
        snprintf(query + strlen(query), sizeof(query) - strlen(query), "%s%s", i > first ? " " : "", argv[i]);
    }

    if (!query[0])
    {
        printf("usage: find [-v] WORDS   find files by meaning, e.g. find invoice from last month\n");
        return 1;
    }

    if (embed_load(&model, EMBED_MODEL) != 0)
    {
        printf("find: cannot load the search model %s\n", EMBED_MODEL);
        return 1;
    }

    int added = 0;
    int removed = 0;

    search_load(&index_data);

    if (search_refresh(&index_data, &model, &added, &removed))
    {
        search_save(&index_data);
    }

    char words[QUERY_WORDS][32];
    int count = embed_words(query, strlen(query), words, QUERY_WORDS);
    char meaning[512] = "";
    filter_t filter;
    long now = realtime();

    memset(&filter, 0, sizeof(filter));

    for (int i = 0; i < count;)
    {
        int used = now > 0 ? date_phrase(words, count, i, now, &filter) : 0;

        if (used)
        {
            i += used;
            continue;
        }

        unsigned int type = type_word(words[i]);

        if (type != TYPE_OTHER)
        {
            filter.type = type;
        }

        snprintf(meaning + strlen(meaning), sizeof(meaning) - strlen(meaning), "%s ", alias_of(words[i]));
        i++;
    }

    float q[EMBED_DIM_MAX];
    float v[INDEX_VECTOR];
    int found = 0;

    embed_text(&model, meaning[0] ? meaning : query, strlen(meaning[0] ? meaning : query), q);

    for (int i = 0; i < index_data.count; i++)
    {
        index_entry_t *e = &index_data.entries[i];

        if ((filter.to && ((long)e->modified < filter.from || (long)e->modified >= filter.to)) ||
            (filter.type && e->type != filter.type))
        {
            continue;
        }

        search_vector(e, v);
        results[found].index = i;
        results[found].meaning = embed_dot(q, v, model.dim < INDEX_VECTOR ? model.dim : INDEX_VECTOR);
        results[found].bonus = name_bonus(e, words, count);
        results[found].score = results[found].meaning + results[found].bonus;
        found++;
    }

    for (int i = 1; i < found; i++)
    {
        result_t r = results[i];
        int j = i - 1;

        while (j >= 0 && results[j].score < r.score)
        {
            results[j + 1] = results[j];
            j--;
        }

        results[j + 1] = r;
    }

    if (filter.type && filter.to)
    {
        printf("find: %s files from %s\n", search_type_name(filter.type), filter.label);
    }
    else if (filter.type)
    {
        printf("find: %s files\n", search_type_name(filter.type));
    }
    else if (filter.to)
    {
        printf("find: from %s\n", filter.label);
    }

    int shown = 0;

    for (int i = 0; i < found && shown < RESULTS; i++)
    {
        int filtered_best = shown == 0 && (filter.to || filter.type);

        if (!filtered_best && (results[i].score < MIN_SCORE || results[i].score < results[0].score - BEST_GAP))
        {
            break;
        }

        index_entry_t *e = &index_data.entries[results[i].index];
        int percent = (int)(results[i].score * 100 + 0.5f);

        printf("  %3d%%  %s  ", percent > 100 ? 100 : percent, e->path);
        print_date(e->modified);
        printf("\n");

        if (e->snippet[0])
        {
            printf("        \"%.70s%s\"\n", e->snippet, strlen(e->snippet) > 70 ? "..." : "");
        }

        if (verbose)
        {
            printf("        meaning %.2f + name %.2f, %s file\n", (double)results[i].meaning, (double)results[i].bonus,
                   search_type_name(e->type));
        }

        shown++;
    }

    if (shown == 0)
    {
        printf("find: nothing in %s matches \"%s\" (%d files searched)\n", INDEX_ROOT, query, index_data.count);
        return 1;
    }

    return 0;
}
