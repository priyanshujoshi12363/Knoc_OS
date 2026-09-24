#include "ulib.h"
#include "nn.h"

double nn_round_even(double value)
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
    double scaled = nn_round_even(value * 127.0);

    return scaled < 0 ? 0 : scaled > 127 ? 127 : (int)scaled;
}

double nn_log2(double value)
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

double nn_exp(double value)
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

static int xq[NN_WIDTH_MAX];
static int next_xq[NN_WIDTH_MAX];
static double logits[NN_CLASSES_MAX * NN_HEADS_MAX];

static void forward(const nn_model_t *model, const double *inputs)
{
    unsigned int width = model->inputs;

    for (unsigned int i = 0; i < model->inputs; i++)
    {
        xq[i] = quantize(inputs[i]);
    }

    for (unsigned int l = 0; l < model->layer_count; l++)
    {
        const nn_layer_t *layer = &model->layers[l];

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
                double scaled = nn_round_even((real > 0 ? real : 0) / layer->out_scale);
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
}

void nn_logits(const nn_model_t *model, const double *inputs, double *out)
{
    forward(model, inputs);

    for (unsigned int i = 0; i < model->layers[model->layer_count - 1].outputs; i++)
    {
        out[i] = logits[i];
    }
}

void nn_infer(const nn_model_t *model, const double *inputs, int *best_class, double *confidence)
{
    forward(model, inputs);

    unsigned int start = 0;

    for (unsigned int h = 0; h < model->head_count; h++)
    {
        double highest = logits[start];
        int best = 0;

        for (unsigned int c = 1; c < model->classes[h]; c++)
        {
            if (logits[start + c] > highest)
            {
                highest = logits[start + c];
                best = (int)c;
            }
        }

        double total = 0.0;

        for (unsigned int c = 0; c < model->classes[h]; c++)
        {
            total += nn_exp(logits[start + c] - highest);
        }

        best_class[h] = best;
        confidence[h] = 1.0 / total;

        start += model->classes[h];
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

int nn_load(nn_model_t *model, const char *path)
{
    file_stat_t info;

    if (stat(path, &info) != 0)
    {
        return -1;
    }

    unsigned char *data = mem_alloc(info.size);
    int fd = open(path, O_READ);

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

    for (int i = 0; i < 6; i++)
    {
        model->config[i] = read_u32(&p);
    }

    model->head_count = read_u32(&p);

    if (model->head_count == 0 || model->head_count > NN_HEADS_MAX)
    {
        return -1;
    }

    for (unsigned int h = 0; h < model->head_count; h++)
    {
        model->classes[h] = read_u32(&p);

        if (model->classes[h] > NN_CLASSES_MAX)
        {
            return -1;
        }

        for (unsigned int c = 0; c < model->classes[h]; c++)
        {
            unsigned int length = *p++;

            for (unsigned int i = 0; i < length && i < 23; i++)
            {
                model->names[h][c][i] = (char)p[i];
            }

            model->names[h][c][length < 23 ? length : 23] = 0;
            p += length;
        }
    }

    model->layer_count = read_u32(&p);

    if (model->layer_count > NN_LAYERS_MAX)
    {
        return -1;
    }

    for (unsigned int l = 0; l < model->layer_count; l++)
    {
        nn_layer_t *layer = &model->layers[l];

        layer->inputs = read_u32(&p);
        layer->outputs = read_u32(&p);
        layer->relu = read_u32(&p);
        layer->in_scale = read_f32(&p);
        layer->out_scale = read_f32(&p);

        if (layer->outputs > NN_WIDTH_MAX || layer->inputs > NN_WIDTH_MAX)
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

        if (l == 0)
        {
            model->inputs = layer->inputs;
        }

        layer->weights = (const signed char *)p;
        p += (unsigned long)layer->outputs * layer->inputs;
    }

    return p <= data + info.size ? 0 : -1;
}
