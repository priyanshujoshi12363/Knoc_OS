#include "ulib.h"
#include "nn.h"

#define MODEL_PATH "/models/health.knm"
#define WINDOW 10
#define METRICS 15
#define FEATURES (METRICS * 3 + 3)
#define LABELS 6
#define MIN_CONFIDENCE 0.8
#define CONFIRM_SECONDS 3
#define CALM_SECONDS 5
#define LEAK_GROWING_SECONDS 6

static const char *label_text[LABELS] = {"normal", "memory leak", "CPU hog", "disk thrashing",
                                         "spawn storm", "disk filling up"};

static nn_model_t model;
static telemetry_sample_t window[WINDOW];
static double inputs[FEATURES];
static double rows[WINDOW][METRICS];

static double clip(double value)
{
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

static double scaled_log(double value, double top)
{
    return clip(nn_log2(1 + (value > 0 ? value : 0)) / top);
}

static double tanh_of(double value)
{
    double magnitude = value < 0 ? -value : value;
    double e = nn_exp(-2 * magnitude);
    double result = (1 - e) / (1 + e);

    return value < 0 ? -result : result;
}

static double squash(double value, double scale)
{
    return 0.5 + 0.5 * tanh_of(value / scale);
}

static void metrics(const telemetry_sample_t *s, double *out)
{
    double ram_used = s->ram_total_kib ? 1 - (double)s->ram_free_kib / s->ram_total_kib : 0;
    double disk_used = s->disk_total_kib ? 1 - (double)s->disk_free_kib / s->disk_total_kib : 0;

    out[0] = clip(s->cpu_busy / 100.0);
    out[1] = clip(s->top_cpu / 100.0);
    out[2] = scaled_log(s->switches, 16);
    out[3] = scaled_log(s->syscalls, 14);
    out[4] = scaled_log(s->spawns, 6);
    out[5] = scaled_log(s->crashes, 4);
    out[6] = scaled_log(s->disk_reads, 14);
    out[7] = scaled_log(s->disk_writes, 14);
    out[8] = scaled_log(s->disk_wait, 8);
    out[9] = clip(ram_used);
    out[10] = scaled_log((double)s->user_memory_kib, 21);
    out[11] = clip(disk_used);
    out[12] = scaled_log(s->top_mem_kib, 21);
    out[13] = scaled_log(s->top_sys, 14);
    out[14] = scaled_log(s->top_spawn, 6);
}

static double growth(double first, double last)
{
    return (last - first) / (WINDOW - 1);
}

static void compute_features(void)
{
    int k = 0;

    for (int i = 0; i < WINDOW; i++)
    {
        metrics(&window[i], rows[i]);
    }

    for (int m = 0; m < METRICS; m++)
    {
        double sum = 0;
        double highest = 0;

        for (int i = 0; i < WINDOW; i++)
        {
            sum += rows[i][m];
            highest = rows[i][m] > highest ? rows[i][m] : highest;
        }

        double early = (rows[0][m] + rows[1][m] + rows[2][m]) / 3;
        double late = (rows[WINDOW - 3][m] + rows[WINDOW - 2][m] + rows[WINDOW - 1][m]) / 3;
        double trend = (late - early) * 4;

        trend = trend < -1 ? -1 : trend > 1 ? 1 : trend;
        inputs[k++] = sum / WINDOW;
        inputs[k++] = highest;
        inputs[k++] = 0.5 + 0.5 * trend;
    }

    const telemetry_sample_t *first = &window[0];
    const telemetry_sample_t *last = &window[WINDOW - 1];
    double disk_first = (double)first->disk_total_kib - (double)first->disk_free_kib;
    double disk_last = (double)last->disk_total_kib - (double)last->disk_free_kib;

    inputs[k++] = squash(growth((double)first->user_memory_kib, (double)last->user_memory_kib), 1024);
    inputs[k++] = squash(growth(disk_first, disk_last), 512);
    inputs[k++] = squash(growth(first->top_mem_kib, last->top_mem_kib), 1024);
}

static int read_window(void)
{
    for (int i = 0; i < WINDOW; i++)
    {
        if (telemetry((unsigned long)(WINDOW - 1 - i), &window[i]) != 0)
        {
            return -1;
        }
    }

    return window[WINDOW - 1].seq - window[0].seq == WINDOW - 1 ? 0 : -1;
}

static int steady_growth(void)
{
    int growing = 0;

    for (int i = 1; i < WINDOW; i++)
    {
        if (window[i].top_mem_kib > window[i - 1].top_mem_kib &&
            strcmp(window[i].top_mem_name, window[i - 1].top_mem_name) == 0)
        {
            growing++;
        }
    }

    return growing >= LEAK_GROWING_SECONDS;
}

static const char *culprit(int label)
{
    const telemetry_sample_t *s = &window[WINDOW - 1];

    if (label == 1)
    {
        return s->top_mem_name;
    }

    if (label == 2)
    {
        return s->top_cpu_name;
    }

    if (label == 4)
    {
        return s->top_spawn_name;
    }

    return s->top_sys_name;
}

static void print_detail(int label)
{
    const telemetry_sample_t *first = &window[0];
    const telemetry_sample_t *last = &window[WINDOW - 1];

    if (label == 1)
    {
        long rate = ((long)last->top_mem_kib - (long)first->top_mem_kib) / (WINDOW - 1);

        print("+");
        print_uint(rate > 0 ? (unsigned long)rate : 0);
        print(" KiB/s, now ");
        print_uint(last->top_mem_kib / 1024);
        print(" MiB");
    }
    else if (label == 2)
    {
        print_uint(last->top_cpu);
        print("% CPU");
    }
    else if (label == 3)
    {
        print_uint(last->disk_reads + last->disk_writes);
        print(" disk requests/s");
    }
    else if (label == 4)
    {
        print_uint(last->spawns);
        print(" programs started/s");
    }
    else
    {
        print_uint(last->disk_free_kib / 1024);
        print(" MiB free on disk");
    }
}

int main(void)
{
    int streak[LABELS];
    int active[LABELS];
    int calm[LABELS];
    char who[LABELS][TELEMETRY_NAME_MAX];
    unsigned int last_seq = 0;

    if (nn_load(&model, MODEL_PATH) != 0 || model.inputs != FEATURES || model.head_count != 1 ||
        model.classes[0] != LABELS || model.config[0] != FEATURES || model.config[1] != WINDOW)
    {
        print("[HEALTH] no health model at " MODEL_PATH ", the anomaly detector is off\n");
        return 1;
    }

    memset(streak, 0, sizeof(streak));
    memset(active, 0, sizeof(active));
    memset(calm, 0, sizeof(calm));

    print("[HEALTH] anomaly detector running (watching CPU, memory, disk, processes)\n");

    while (1)
    {
        sleep(50);

        if (read_window() != 0 || window[WINDOW - 1].seq == last_seq)
        {
            continue;
        }

        last_seq = window[WINDOW - 1].seq;
        compute_features();

        int best;
        double confidence;

        nn_infer(&model, inputs, &best, &confidence);

        for (int label = 1; label < LABELS; label++)
        {
            if (best == label && confidence >= MIN_CONFIDENCE && (label != 1 || steady_growth()))
            {
                streak[label]++;
                calm[label] = 0;
            }
            else
            {
                streak[label] = 0;
                calm[label]++;
            }

            if (!active[label] && streak[label] >= CONFIRM_SECONDS)
            {
                const char *name = culprit(label);

                active[label] = 1;
                strcpy(who[label], name[0] ? name : "unknown");
                print("[HEALTH] ");
                print(label_text[label]);
                print(" in ");
                print(who[label]);
                print(" (");
                print_detail(label);
                print(", ");
                print_uint((unsigned long)nn_round_even(confidence * 100));
                print("% sure)\n");
                graph_record(GRAPH_KIND_PROGRAM, who[label], GRAPH_REL_ANOMALY, GRAPH_KIND_DIAGNOSIS,
                             label_text[label], (unsigned int)nn_round_even(confidence * 100));
            }
            else if (active[label] && calm[label] >= CALM_SECONDS)
            {
                active[label] = 0;
                print("[HEALTH] ");
                print(label_text[label]);
                print(" in ");
                print(who[label]);
                print(" is over\n");
            }
        }
    }
}
