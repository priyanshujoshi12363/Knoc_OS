#include "input.h"
#include "process.h"
#include "spinlock.h"
#include "uart.h"
#include "virtio_gpu.h"
#include "fbcon.h"

#define QUEUE_SIZE 256
#define ABS_RANGE 32768

static const char plain[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 127, '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

static const char shifted[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 127, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static input_event_t queue[QUEUE_SIZE];
static uint32_t head;
static uint32_t tail;
static int owner;
static uint32_t modifiers;
static int capslock;
static int32_t pointer_x;
static int32_t pointer_y;
static int moved;
static char channel;
static spinlock_t lock = SPINLOCK_INIT;

static void push(uint16_t type, uint16_t code, int32_t value, uint32_t text)
{
    uint32_t next = (head + 1) % QUEUE_SIZE;

    if (next == tail)
    {
        return;
    }

    queue[head].type = type;
    queue[head].code = code;
    queue[head].value = value;
    queue[head].x = pointer_x;
    queue[head].y = pointer_y;
    queue[head].modifiers = modifiers;
    queue[head].text = text;
    head = next;
}

static uint32_t modifier_of(uint16_t code)
{
    if (code == 42 || code == 54)
    {
        return INPUT_SHIFT;
    }

    if (code == 29 || code == 97)
    {
        return INPUT_CTRL;
    }

    if (code == 56 || code == 100)
    {
        return INPUT_ALT;
    }

    if (code == 125 || code == 126)
    {
        return INPUT_SUPER;
    }

    return 0;
}

static uint32_t text_of(uint16_t code)
{
    if (code >= 128)
    {
        return 0;
    }

    int shift = (modifiers & INPUT_SHIFT) != 0;
    char c = shift ? shifted[code] : plain[code];

    if (capslock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
    {
        c = shift ? plain[code] : shifted[code];
    }

    return (uint32_t)(uint8_t)c;
}

static void console_sequence(const char *text)
{
    while (*text)
    {
        uart_inject(*text++);
    }
}

static void console_key(uint16_t code)
{
    if (code == KEY_ESC)
    {
        fbcon_reveal();
    }

    switch (code)
    {
    case KEY_UP:
        console_sequence("\033[A");
        return;
    case KEY_DOWN:
        console_sequence("\033[B");
        return;
    case KEY_RIGHT:
        console_sequence("\033[C");
        return;
    case KEY_LEFT:
        console_sequence("\033[D");
        return;
    case KEY_HOME:
        console_sequence("\033[H");
        return;
    case KEY_END:
        console_sequence("\033[F");
        return;
    case KEY_DELETE:
        console_sequence("\033[3~");
        return;
    case KEY_PAGEUP:
        console_sequence("\033[5~");
        return;
    case KEY_PAGEDOWN:
        console_sequence("\033[6~");
        return;
    default:
        break;
    }

    uint32_t c = text_of(code);

    if (!c)
    {
        return;
    }

    if (modifiers & INPUT_CTRL)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        {
            c = (c | 0x20) - 'a' + 1;
        }
        else if (c == '[')
        {
            c = 27;
        }
    }

    uart_inject((char)c);
}

static int has_owner(void)
{
    if (owner != 0 && !process_alive(owner))
    {
        owner = 0;
    }

    return owner != 0;
}

void input_key(uint16_t code, int32_t value)
{
    uint64_t interrupts = spin_lock(&lock);
    uint32_t mod = modifier_of(code);

    if (mod)
    {
        modifiers = value ? modifiers | mod : modifiers & ~mod;
    }

    if (code == 58 && value == 1)
    {
        capslock = !capslock;
    }

    int gui = has_owner();

    if (gui)
    {
        push(INPUT_KEY, code, value, value ? text_of(code) : 0);
    }

    spin_unlock(&lock, interrupts);

    if (gui)
    {
        process_wake(&channel);
    }
    else if (value && !mod)
    {
        console_key(code);
    }
}

void input_absolute(int axis, int32_t value)
{
    if (!gpu_ready())
    {
        return;
    }

    if (value < 0)
    {
        value = 0;
    }

    if (axis == 0)
    {
        pointer_x = (int32_t)((int64_t)value * gpu_width() / ABS_RANGE);
    }
    else
    {
        pointer_y = (int32_t)((int64_t)value * gpu_height() / ABS_RANGE);
    }

    moved = 1;
}

void input_button(uint16_t code, int32_t value)
{
    uint16_t button = code == 0x110 ? BUTTON_LEFT : code == 0x111 ? BUTTON_RIGHT : code == 0x112 ? BUTTON_MIDDLE : 0;

    if (!button)
    {
        return;
    }

    uint64_t interrupts = spin_lock(&lock);

    if (has_owner())
    {
        push(INPUT_BUTTON, button, value, 0);
    }

    spin_unlock(&lock, interrupts);
    process_wake(&channel);
}

void input_wheel(int32_t value)
{
    uint64_t interrupts = spin_lock(&lock);

    if (has_owner())
    {
        push(INPUT_WHEEL, 0, value, 0);
    }

    spin_unlock(&lock, interrupts);
    process_wake(&channel);
}

void input_sync(void)
{
    if (!moved)
    {
        return;
    }

    moved = 0;

    uint64_t interrupts = spin_lock(&lock);
    int gui = has_owner();

    if (gui)
    {
        if (head != tail && queue[(head + QUEUE_SIZE - 1) % QUEUE_SIZE].type == INPUT_MOVE)
        {
            queue[(head + QUEUE_SIZE - 1) % QUEUE_SIZE].x = pointer_x;
            queue[(head + QUEUE_SIZE - 1) % QUEUE_SIZE].y = pointer_y;
        }
        else
        {
            push(INPUT_MOVE, 0, 0, 0);
        }
    }

    spin_unlock(&lock, interrupts);

    if (gui)
    {
        gpu_cursor_move((uint32_t)pointer_x, (uint32_t)pointer_y);
        process_wake(&channel);
    }
}

int64_t input_read(input_event_t *events, uint32_t max, uint64_t timeout)
{
    uint64_t interrupts = irq_save();

    if (head == tail && timeout > 0)
    {
        process_block(&channel, timeout);
    }

    irq_restore(interrupts);
    interrupts = spin_lock(&lock);

    uint32_t count = 0;

    while (count < max && head != tail)
    {
        events[count++] = queue[tail];
        tail = (tail + 1) % QUEUE_SIZE;
    }

    spin_unlock(&lock, interrupts);
    return count;
}

void input_claim(int pid)
{
    uint64_t interrupts = spin_lock(&lock);

    if (owner != pid)
    {
        owner = pid;
        head = tail = 0;
        modifiers = 0;
    }

    spin_unlock(&lock, interrupts);
}

void input_release(int pid)
{
    uint64_t interrupts = spin_lock(&lock);

    if (owner == pid)
    {
        owner = 0;
        head = tail = 0;
    }

    spin_unlock(&lock, interrupts);
}

int input_owner(void)
{
    return owner;
}
