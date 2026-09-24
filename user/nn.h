#ifndef NN_H
#define NN_H

#define NN_LAYERS_MAX 4
#define NN_HEADS_MAX 2
#define NN_CLASSES_MAX 16
#define NN_WIDTH_MAX 2048

typedef struct nn_layer
{
    unsigned int inputs;
    unsigned int outputs;
    unsigned int relu;
    double in_scale;
    double out_scale;
    float *w_scale;
    int *bias;
    const signed char *weights;
} nn_layer_t;

typedef struct nn_model
{
    unsigned int head_count;
    unsigned int classes[NN_HEADS_MAX];
    char names[NN_HEADS_MAX][NN_CLASSES_MAX][24];
    unsigned int layer_count;
    unsigned int inputs;
    unsigned int config[6];
    nn_layer_t layers[NN_LAYERS_MAX];
} nn_model_t;

int nn_load(nn_model_t *model, const char *path);
void nn_infer(const nn_model_t *model, const double *inputs, int *best_class, double *confidence);
double nn_round_even(double value);
double nn_log2(double value);
double nn_exp(double value);

#endif
