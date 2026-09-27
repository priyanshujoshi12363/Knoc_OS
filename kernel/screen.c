#include "screen.h"
#include "virtio_gpu.h"
#include "fbcon.h"
#include "process.h"
#include "spinlock.h"
#include "uart.h"
#include "input.h"

static int owner;
static spinlock_t lock = SPINLOCK_INIT;

int screen_info(screen_info_t *info)
{
    if (!gpu_ready())
    {
        return E_NODEV;
    }

    info->width = gpu_width();
    info->height = gpu_height();
    info->stride = gpu_width() * 4;
    info->format = SCREEN_FORMAT_XRGB8888;
    info->owner = owner;
    info->console_columns = fbcon_columns();
    info->console_rows = fbcon_rows();
    info->reserved = 0;
    return 0;
}

int64_t screen_map(void)
{
    if (!gpu_ready())
    {
        return E_NODEV;
    }

    int me = process_owner_pid();
    uint64_t interrupts = spin_lock(&lock);

    if (owner != 0 && owner != me)
    {
        spin_unlock(&lock, interrupts);
        return E_BUSY;
    }

    int first = owner == 0;

    owner = me;
    spin_unlock(&lock, interrupts);

    if (first)
    {
        if (process_map_device(USER_SCREEN_BASE, (uintptr_t)gpu_framebuffer(), gpu_framebuffer_bytes()) < 0)
        {
            owner = 0;
            return E_NOMEM;
        }

        fbcon_pause();
        uart_puts("[SCREEN] ");
        uart_puts(process_current_name());
        uart_puts(" took the screen\n");
    }

    return (int64_t)USER_SCREEN_BASE;
}

int screen_flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (owner == 0 || owner != process_owner_pid())
    {
        return E_PERM;
    }

    return gpu_flush(x, y, width, height) == 0 ? 0 : E_INVAL;
}

int64_t screen_input(input_event_t *events, uint32_t max, uint64_t timeout)
{
    int me = process_owner_pid();

    if (owner == 0 || owner != me)
    {
        return E_PERM;
    }

    input_claim(me);
    return input_read(events, max, timeout);
}

int screen_cursor(const uint32_t *pixels, uint32_t hot_x, uint32_t hot_y)
{
    if (owner == 0 || owner != process_owner_pid())
    {
        return E_PERM;
    }

    return gpu_cursor_set(pixels, hot_x, hot_y) == 0 ? 0 : E_NODEV;
}

void screen_release(int pid)
{
    uint64_t interrupts = spin_lock(&lock);
    int mine = owner != 0 && owner == pid;

    if (mine)
    {
        owner = 0;
    }

    spin_unlock(&lock, interrupts);

    if (mine)
    {
        input_release(pid);
        gpu_cursor_hide();
        fbcon_resume();
        uart_puts("[SCREEN] the console is back on the screen\n");
    }
}
