#ifndef KNOC_EMBED_H
#define KNOC_EMBED_H

#define EMBED_MODEL "/models/knocembed.knm"
#define EMBED_DIM_MAX 128

typedef struct embed_model
{
    unsigned int buckets;
    unsigned int dim;
    unsigned int min_n;
    unsigned int max_n;
    unsigned int max_words;
    unsigned int max_word;
    float *scales;
    signed char *table;
} embed_model_t;

int embed_load(embed_model_t *model, const char *path);
int embed_text(const embed_model_t *model, const char *text, unsigned long length, float *out);
float embed_dot(const float *a, const float *b, unsigned int dim);
int embed_words(const char *text, unsigned long length, char words[][32], int max);

#endif
