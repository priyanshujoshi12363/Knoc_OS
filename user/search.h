#ifndef KNOC_SEARCH_H
#define KNOC_SEARCH_H

#include "embed.h"

#define INDEX_DIR "/var/index"
#define INDEX_FILE INDEX_DIR "/files.idx"
#define INDEX_ROOT "/home"
#define INDEX_VECTOR 64
#define INDEX_PATH_MAX 160
#define INDEX_SNIPPET 100
#define INDEX_FILES_MAX 4000

enum
{
    TYPE_OTHER,
    TYPE_TEXT,
    TYPE_DOCUMENT,
    TYPE_IMAGE,
    TYPE_AUDIO,
    TYPE_VIDEO,
    TYPE_CODE,
    TYPE_ARCHIVE,
    TYPE_DATA,
    TYPE_COUNT
};

typedef struct index_entry
{
    char path[INDEX_PATH_MAX];
    unsigned long modified;
    unsigned long size;
    unsigned int type;
    float scale;
    signed char vector[INDEX_VECTOR];
    char snippet[INDEX_SNIPPET];
    unsigned int seen;
} index_entry_t;

typedef struct search_index
{
    index_entry_t *entries;
    int count;
    int room;
    unsigned long updated;
} search_index_t;

const char *search_type_name(unsigned int type);
unsigned int search_type_of(const char *name);
int search_load(search_index_t *index);
int search_save(const search_index_t *index);
int search_refresh(search_index_t *index, const embed_model_t *model, int *added, int *removed);
void search_vector(const index_entry_t *entry, float *out);

#endif
