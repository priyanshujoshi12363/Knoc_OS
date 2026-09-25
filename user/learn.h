#ifndef LEARN_H
#define LEARN_H

#include "nn.h"

#define LEARN_HIDDEN 96
#define LEARN_NAME_BINS 128
#define LEARN_DIM (LEARN_HIDDEN + LEARN_NAME_BINS)
#define LEARN_DIR "/home/.organize"

void learn_features(const nn_model_t *model, const char *name, double *x);
int learn_load(void);
int learn_predict(const double *x, int type, char *folder, double *confidence);
int learn_add(const char *name, unsigned long size, const char *folder, int type, const double *x);
int learn_train(void);
void learn_forget(void);
void learn_list(void);

#endif
