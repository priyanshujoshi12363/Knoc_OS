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
#define LEAK_STOP_SECONDS 10
#define DISK_GROWING_SECONDS 5
#define DISK_GROWTH_KIB 512
#define DISK_FULL_SECONDS 60
#define MODE_PATH "/etc/health.mode"
#define COOLDOWN_SECONDS 60
#define WATCH_MAX 16
#define DENIED_WINDOW 20
#define DENIED_ALERT 3

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

static double used(unsigned long total, unsigned long free)
{
    return (double)total - (double)free;
}

static void metrics(const telemetry_sample_t *s, const telemetry_sample_t *base, double *out)
{
    double ram_change = used(s->ram_total_kib, s->ram_free_kib) - used(base->ram_total_kib, base->ram_free_kib);
    double disk_change =
        used(s->disk_total_kib, s->disk_free_kib) - used(base->disk_total_kib, base->disk_free_kib);

    out[0] = clip(s->cpu_busy / 100.0);
    out[1] = clip(s->top_cpu / 100.0);
    out[2] = scaled_log(s->switches, 16);
    out[3] = scaled_log(s->syscalls, 14);
    out[4] = scaled_log(s->spawns, 6);
    out[5] = scaled_log(s->crashes, 4);
    out[6] = scaled_log(s->disk_reads, 14);
    out[7] = scaled_log(s->disk_writes, 14);
    out[8] = scaled_log(s->disk_wait, 8);
    out[9] = squash(ram_change, 16384);
    out[10] = scaled_log((double)s->user_memory_kib, 21);
    out[11] = squash(disk_change, 4096);
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
        metrics(&window[i], &window[0], rows[i]);
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

static int disk_filling(void)
{
    int growing = 0;

    for (int i = 1; i < WINDOW; i++)
    {
        growing += window[i].disk_free_kib < window[i - 1].disk_free_kib;
    }

    return growing >= DISK_GROWING_SECONDS &&
           window[0].disk_free_kib >= window[WINDOW - 1].disk_free_kib + DISK_GROWTH_KIB;
}

static const char *top_of(const telemetry_sample_t *s, int label, int *pid)
{
    if (label == 1)
    {
        *pid = s->top_mem_pid;
        return s->top_mem_name;
    }

    if (label == 2)
    {
        *pid = s->top_cpu_pid;
        return s->top_cpu_name;
    }

    if (label == 4)
    {
        *pid = s->top_spawn_pid;
        return s->top_spawn_name;
    }

    *pid = s->top_disk_pid;
    return s->top_disk_name;
}

static const char *blame(int label, int *pid)
{
    const char *best = "";
    int best_count = 0;

    *pid = -1;

    for (int i = WINDOW - 1; i >= 0; i--)
    {
        int candidate_pid;
        const char *name = top_of(&window[i], label, &candidate_pid);
        int count = 0;

        if (name[0] == 0)
        {
            continue;
        }

        for (int j = 0; j < WINDOW; j++)
        {
            int other_pid;

            count += strcmp(top_of(&window[j], label, &other_pid), name) == 0;
        }

        if (count > best_count)
        {
            best = name;
            best_count = count;
            *pid = candidate_pid;
        }
    }

    return best;
}

static const char *culprit(int label)
{
    int pid;

    return blame(label, &pid);
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

static int culprit_pid(int label)
{
    int pid;

    blame(label, &pid);
    return pid;
}

static int recover_mode(void)
{
    char text[16];
    int fd = open(MODE_PATH, O_READ);

    if (fd < 0)
    {
        return 1;
    }

    long got = read(fd, text, sizeof(text) - 1);

    close(fd);
    return !(got >= 5 && memcmp_bytes(text, "watch", 5) == 0);
}

static int find_process(int pid, process_info_t *info)
{
    for (unsigned long i = 0; ps(i, info) == 0; i++)
    {
        if (info->pid == pid)
        {
            return 1;
        }
    }

    return 0;
}

static int running(int pid, int *foreground)
{
    process_info_t info;

    if (!find_process(pid, &info))
    {
        return 0;
    }

    *foreground = (info.flags & PROCESS_FLAG_FOREGROUND) != 0;
    return 1;
}

static int protected_program(const char *name)
{
    return strcmp(name, "knocsh") == 0 || strcmp(name, "healthd") == 0 || strcmp(name, "unknown") == 0;
}

static int disk_full_soon(void)
{
    const telemetry_sample_t *first = &window[0];
    const telemetry_sample_t *last = &window[WINDOW - 1];

    if (last->disk_free_kib * 10 < last->disk_total_kib)
    {
        return 1;
    }

    if (first->disk_free_kib <= last->disk_free_kib)
    {
        return 0;
    }

    unsigned long rate = (first->disk_free_kib - last->disk_free_kib) / (WINDOW - 1);

    return rate > 0 && last->disk_free_kib / rate < DISK_FULL_SECONDS;
}

static int ready_to_act(int label, int seconds)
{
    if (label == 1)
    {
        return seconds >= LEAK_STOP_SECONDS;
    }

    if (label == 5)
    {
        return disk_full_soon();
    }

    return 1;
}

static void report_action(const char *what, const char *name, int pid, int label, int result)
{
    print("[HEALTH] ");

    if (result != 0)
    {
        print("could not fix the ");
        print(label_text[label]);
        print(" in ");
        print(name);
        print("\n");
        return;
    }

    print("recovered: ");
    print(what);
    print(" ");
    print(name);
    print(" (pid ");
    print_uint((unsigned long)pid);
    print(") because of the ");
    print(label_text[label]);
    print("\n");
}

static void act(int label, const char *name, int pid)
{
    if (label == 2 || label == 3)
    {
        process_info_t info;

        if (find_process(pid, &info) && info.process_class == CLASS_BACKGROUND)
        {
            return;
        }

        int result = setclass(pid, CLASS_BACKGROUND);

        report_action("moved to background priority:", name, pid, label, result);

        if (result == 0)
        {
            graph_record(GRAPH_KIND_ACTOR, "healthd", GRAPH_REL_LOWERED, GRAPH_KIND_PROGRAM, name, 100);
        }

        return;
    }

    int result = kill(pid);

    report_action("stopped", name, pid, label, result);

    if (result == 0)
    {
        graph_record(GRAPH_KIND_ACTOR, "healthd", GRAPH_REL_STOPPED, GRAPH_KIND_PROGRAM, name, 100);
    }
}

typedef struct watched
{
    int pid;
    char name[INFO_NAME_MAX];
    unsigned int last;
    unsigned int history[DENIED_WINDOW];
    int alerted;
    int seen;
} watched_t;

static watched_t watched[WATCH_MAX];
static unsigned int watch_tick;

static watched_t *watch_slot(const process_info_t *info)
{
    watched_t *free_slot = 0;

    for (int i = 0; i < WATCH_MAX; i++)
    {
        if (watched[i].pid == info->pid)
        {
            return &watched[i];
        }

        if (watched[i].pid == 0 && !free_slot)
        {
            free_slot = &watched[i];
        }
    }

    if (free_slot)
    {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->pid = info->pid;
        strcpy(free_slot->name, info->name);
        free_slot->last = info->denied;
    }

    return free_slot;
}

static void permission_watch(void)
{
    process_info_t info;
    unsigned int slot = watch_tick++ % DENIED_WINDOW;

    for (int i = 0; i < WATCH_MAX; i++)
    {
        watched[i].seen = 0;
    }

    for (unsigned long i = 0; ps(i, &info) == 0; i++)
    {
        if (!info.user)
        {
            continue;
        }

        watched_t *w = watch_slot(&info);

        if (!w)
        {
            continue;
        }

        unsigned int total = 0;

        w->seen = 1;
        w->history[slot] = info.denied - w->last;
        w->last = info.denied;

        for (int h = 0; h < DENIED_WINDOW; h++)
        {
            total += w->history[h];
        }

        if (total < DENIED_ALERT || w->alerted)
        {
            continue;
        }

        w->alerted = 1;
        print("[SECURITY] ");
        print(w->name);
        print(" keeps asking for things it has no permission for (");
        print_uint(total);
        print(" denied calls in 10 s)\n");
        graph_record(GRAPH_KIND_PROGRAM, w->name, GRAPH_REL_ANOMALY, GRAPH_KIND_DIAGNOSIS,
                     "repeated permission denials", 100);

        if (!recover_mode() || protected_program(w->name))
        {
            continue;
        }

        if (kill(w->pid) == 0)
        {
            print("[SECURITY] recovered: stopped ");
            print(w->name);
            print(" (pid ");
            print_uint((unsigned long)w->pid);
            print(")\n");
            graph_record(GRAPH_KIND_ACTOR, "healthd", GRAPH_REL_STOPPED, GRAPH_KIND_PROGRAM, w->name, 100);
        }
    }

    for (int i = 0; i < WATCH_MAX; i++)
    {
        if (!watched[i].seen)
        {
            watched[i].pid = 0;
        }
    }
}

int main(void)
{
    int streak[LABELS];
    int active[LABELS];
    int acted[LABELS];
    int calm[LABELS];
    int pid[LABELS];
    int cooldown[LABELS];
    char who[LABELS][TELEMETRY_NAME_MAX];
    unsigned int last_seq = 0;

    if (nn_load(&model, MODEL_PATH) != 0 || model.inputs != FEATURES || model.head_count != 1 ||
        model.classes[0] != LABELS - 1 || model.config[0] != FEATURES || model.config[1] != WINDOW ||
        model.config[2] != 2 || model.config[3] != 1)
    {
        print("[HEALTH] no health model at " MODEL_PATH ", the anomaly detector is off\n");
        return 1;
    }

    memset(streak, 0, sizeof(streak));
    memset(active, 0, sizeof(active));
    memset(acted, 0, sizeof(acted));
    memset(calm, 0, sizeof(calm));
    memset(cooldown, 0, sizeof(cooldown));

    print("[HEALTH] anomaly detector running (watching CPU, memory, disk, processes)\n");

    while (1)
    {
        sleep(50);
        permission_watch();

        if (read_window() != 0 || window[WINDOW - 1].seq == last_seq)
        {
            continue;
        }

        last_seq = window[WINDOW - 1].seq;
        compute_features();

        double scores[LABELS - 1];

        nn_logits(&model, inputs, scores);

        for (int label = 1; label < LABELS; label++)
        {
            double confidence = 1.0 / (1.0 + nn_exp(-scores[label - 1]));

            if (cooldown[label] > 0)
            {
                cooldown[label]--;
            }

            int foreground = 0;
            int expected = !running(culprit_pid(label), &foreground) || (label == 2 && foreground);

            if (confidence >= MIN_CONFIDENCE && !expected && (label != 1 || steady_growth()) &&
                (label != 5 || disk_filling()))
            {
                streak[label]++;
                calm[label] = 0;
            }
            else
            {
                streak[label] = 0;
                calm[label]++;
            }

            if (!active[label] && streak[label] >= CONFIRM_SECONDS &&
                (cooldown[label] == 0 || strcmp(culprit(label), who[label]) != 0))
            {
                const char *name = culprit(label);

                active[label] = 1;
                acted[label] = 0;
                pid[label] = culprit_pid(label);
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
                cooldown[label] = COOLDOWN_SECONDS;
                print("[HEALTH] ");
                print(label_text[label]);
                print(" in ");
                print(who[label]);
                print(" is over\n");
            }

            if (active[label] && !acted[label] && streak[label] > 0 && !protected_program(who[label]) &&
                ready_to_act(label, streak[label]))
            {
                acted[label] = 1;

                if (recover_mode())
                {
                    act(label, who[label], pid[label]);
                }
            }
        }
    }
}
