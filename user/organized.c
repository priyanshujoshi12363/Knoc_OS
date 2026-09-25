#include "ulib.h"

#define WATCH_DIR "/home/Downloads"
#define MODE_PATH "/etc/organize.mode"
#define TRACK_MAX 64
#define POLL_TICKS 300
#define STABLE_POLLS 2
#define LEARN_POLLS 5

typedef struct tracked
{
    char name[FILE_NAME_MAX];
    unsigned long size;
    int stable;
    int seen;
} tracked_t;

static tracked_t tracked[TRACK_MAX];

static int auto_on(void)
{
    char text[8];
    int fd = open(MODE_PATH, O_READ);

    if (fd < 0)
    {
        return 0;
    }

    long got = read(fd, text, sizeof(text) - 1);

    close(fd);
    return got >= 2 && text[0] == 'o' && text[1] == 'n';
}

static tracked_t *track(const char *name)
{
    tracked_t *free_slot = 0;

    for (int i = 0; i < TRACK_MAX; i++)
    {
        if (tracked[i].name[0] && strcmp(tracked[i].name, name) == 0)
        {
            return &tracked[i];
        }

        if (!tracked[i].name[0] && !free_slot)
        {
            free_slot = &tracked[i];
        }
    }

    if (free_slot)
    {
        memset(free_slot, 0, sizeof(*free_slot));
        strcpy(free_slot->name, name);
        free_slot->size = (unsigned long)-1;
    }

    return free_slot;
}

static void run_organize(const char *args)
{
    int pid = spawn_args("organize", args);

    if (pid >= 0)
    {
        wait(pid);
    }
}

static void poll_downloads(void)
{
    static dir_entry_t entries[TRACK_MAX];
    int count = 0;

    for (int i = 0; i < TRACK_MAX; i++)
    {
        tracked[i].seen = 0;
    }

    for (unsigned long i = 0; count < TRACK_MAX && readdir(WATCH_DIR, i, &entries[count]) == 0; i++)
    {
        if (entries[count].type == FILE_TYPE_FILE && entries[count].name[0] != '.')
        {
            count++;
        }
    }

    for (int e = 0; e < count; e++)
    {
        dir_entry_t *entry = &entries[e];
        tracked_t *t = track(entry->name);

        if (!t)
        {
            continue;
        }

        t->seen = 1;
        t->stable = t->size == entry->size ? t->stable + 1 : 0;
        t->size = entry->size;

        if (t->stable >= STABLE_POLLS)
        {
            char args[ARGS_MAX];
            unsigned long n = 0;
            const char *prefix = "--quiet " WATCH_DIR " --apply --only ";

            for (unsigned long j = 0; prefix[j] && n < sizeof(args) - 1; j++)
            {
                args[n++] = prefix[j];
            }

            for (unsigned long j = 0; entry->name[j] && n < sizeof(args) - 1; j++)
            {
                args[n++] = entry->name[j];
            }

            args[n] = 0;
            run_organize(args);
            t->name[0] = 0;
        }
    }

    for (int i = 0; i < TRACK_MAX; i++)
    {
        if (!tracked[i].seen)
        {
            tracked[i].name[0] = 0;
        }
    }
}

int main(void)
{
    int was_on = 0;
    int polls = 0;

    while (1)
    {
        int on = auto_on();

        if (on != was_on)
        {
            print(on ? "[ORGANIZE] auto-organize on: watching " WATCH_DIR "\n" : "[ORGANIZE] auto-organize off\n");
            memset(tracked, 0, sizeof(tracked));
            was_on = on;
        }

        if (on)
        {
            poll_downloads();

            if (++polls % LEARN_POLLS == 0)
            {
                run_organize("learn --quiet " WATCH_DIR);
            }
        }

        sleep(POLL_TICKS);
    }
}
