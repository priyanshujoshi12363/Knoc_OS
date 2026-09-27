#include "tty.h"
#include "process.h"
#include "spinlock.h"

#define TTY_BUFFER_SIZE 256

static char buffer[TTY_BUFFER_SIZE];
static volatile uint32_t head;
static volatile uint32_t tail;
static int owner_pid;
static int foreground_pid;
static char input_channel;

void tty_input(char c)
{
    uint64_t enabled = irq_save();
    uint32_t next = (head + 1) % TTY_BUFFER_SIZE;

    if (next != tail)
    {
        buffer[head] = c;
        head = next;
    }

    process_wake(&input_channel);
    irq_restore(enabled);
}

/* Waits until at least one character is there */
int64_t tty_read(char *out, uint64_t length)
{
    uint64_t enabled = irq_save();
    uint64_t count = 0;

    while (head == tail)
    {
        process_block(&input_channel, 0);
    }

    while (count < length && head != tail)
    {
        out[count++] = buffer[tail];
        tail = (tail + 1) % TTY_BUFFER_SIZE;
    }

    irq_restore(enabled);
    return (int64_t)count;
}

void tty_set_owner(int pid)
{
    owner_pid = pid;
}

int tty_has_owner(void)
{
    return owner_pid != 0 && process_alive(owner_pid);
}

void tty_set_foreground(int pid)
{
    foreground_pid = pid;
}

int tty_foreground(void)
{
    return foreground_pid != 0 && process_alive(foreground_pid) ? foreground_pid : 0;
}

int tty_has_input(void)
{
    return head != tail;
}

void tty_wait_input(uint64_t ticks)
{
    uint64_t enabled = irq_save();

    if (head == tail && ticks > 0)
    {
        process_block(&input_channel, ticks);
    }

    irq_restore(enabled);
}
