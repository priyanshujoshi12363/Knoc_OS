#include "pty.h"
#include "process.h"
#include "spinlock.h"
#include "syscall_abi.h"

#define PTY_MAX 8
#define PTY_IN_SIZE 1024
#define PTY_OUT_SIZE 16384

typedef struct pty
{
    int used;
    int owner;
    int foreground;
    char in[PTY_IN_SIZE];
    uint32_t in_head;
    uint32_t in_tail;
    char out[PTY_OUT_SIZE];
    uint32_t out_head;
    uint32_t out_tail;
    char in_channel;
    char out_channel;
} pty_t;

static pty_t ptys[PTY_MAX];
static spinlock_t lock = SPINLOCK_INIT;

static pty_t *get(int id)
{
    return id >= 1 && id <= PTY_MAX && ptys[id - 1].used ? &ptys[id - 1] : 0;
}

int pty_open(int owner)
{
    uint64_t interrupts = spin_lock(&lock);

    for (int i = 0; i < PTY_MAX; i++)
    {
        if (!ptys[i].used)
        {
            pty_t *p = &ptys[i];

            p->used = 1;
            p->owner = owner;
            p->foreground = 0;
            p->in_head = p->in_tail = 0;
            p->out_head = p->out_tail = 0;
            spin_unlock(&lock, interrupts);
            return i + 1;
        }
    }

    spin_unlock(&lock, interrupts);
    return E_NOSPACE;
}

int pty_owned(int id, int owner)
{
    pty_t *p = get(id);

    return p && p->owner == owner;
}

void pty_close(int id)
{
    pty_t *p = get(id);

    if (p)
    {
        p->used = 0;
        process_wake(&p->in_channel);
        process_wake(&p->out_channel);
    }
}

void pty_release_owner(int owner)
{
    for (int i = 0; i < PTY_MAX; i++)
    {
        if (ptys[i].used && ptys[i].owner == owner)
        {
            pty_close(i + 1);
        }
    }
}

int64_t pty_output(int id, const char *data, uint64_t length)
{
    pty_t *p = get(id);

    if (!p)
    {
        return (int64_t)length;
    }

    uint64_t done = 0;

    while (done < length)
    {
        uint64_t interrupts = spin_lock(&lock);

        while (done < length && (p->out_head + 1) % PTY_OUT_SIZE != p->out_tail)
        {
            p->out[p->out_head] = data[done++];
            p->out_head = (p->out_head + 1) % PTY_OUT_SIZE;
        }

        spin_unlock(&lock, interrupts);

        if (done < length)
        {
            if (!process_can_block() || !p->used)
            {
                break;
            }

            interrupts = irq_save();

            if ((p->out_head + 1) % PTY_OUT_SIZE == p->out_tail)
            {
                process_block(&p->out_channel, 10);
            }

            irq_restore(interrupts);
        }
    }

    return (int64_t)length;
}

int64_t pty_take_output(int id, char *out, uint64_t length)
{
    pty_t *p = get(id);

    if (!p)
    {
        return E_NOTFOUND;
    }

    uint64_t interrupts = spin_lock(&lock);
    uint64_t count = 0;

    while (count < length && p->out_tail != p->out_head)
    {
        out[count++] = p->out[p->out_tail];
        p->out_tail = (p->out_tail + 1) % PTY_OUT_SIZE;
    }

    spin_unlock(&lock, interrupts);
    process_wake(&p->out_channel);
    return (int64_t)count;
}

int64_t pty_give_input(int id, const char *data, uint64_t length)
{
    pty_t *p = get(id);

    if (!p)
    {
        return E_NOTFOUND;
    }

    for (uint64_t i = 0; i < length; i++)
    {
        if (data[i] == 3 && p->foreground && process_alive(p->foreground))
        {
            pty_output(id, "^C\n", 3);
            process_kill(p->foreground);
            continue;
        }

        uint64_t interrupts = spin_lock(&lock);
        uint32_t next = (p->in_head + 1) % PTY_IN_SIZE;

        if (next != p->in_tail)
        {
            p->in[p->in_head] = data[i];
            p->in_head = next;
        }

        spin_unlock(&lock, interrupts);
    }

    process_wake(&p->in_channel);
    return (int64_t)length;
}

int64_t pty_read_input(int id, char *out, uint64_t length)
{
    pty_t *p = get(id);

    if (!p)
    {
        return 0;
    }

    uint64_t interrupts = irq_save();

    while (p->used && p->in_head == p->in_tail)
    {
        process_block(&p->in_channel, 0);
    }

    irq_restore(interrupts);
    interrupts = spin_lock(&lock);

    uint64_t count = 0;

    while (count < length && p->in_head != p->in_tail)
    {
        out[count++] = p->in[p->in_tail];
        p->in_tail = (p->in_tail + 1) % PTY_IN_SIZE;
    }

    spin_unlock(&lock, interrupts);
    return (int64_t)count;
}

int pty_has_input(int id)
{
    pty_t *p = get(id);

    return p && p->in_head != p->in_tail;
}

void pty_wait_input(int id, uint64_t ticks)
{
    pty_t *p = get(id);

    if (!p)
    {
        return;
    }

    uint64_t interrupts = irq_save();

    if (p->in_head == p->in_tail && ticks > 0)
    {
        process_block(&p->in_channel, ticks);
    }

    irq_restore(interrupts);
}

void pty_set_foreground(int id, int pid)
{
    pty_t *p = get(id);

    if (p)
    {
        p->foreground = pid;
    }
}
