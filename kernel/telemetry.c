#include "telemetry.h"
#include "process.h"
#include "page.h"
#include "knocfs.h"
#include "virtio_blk.h"
#include "timer.h"
#include "string.h"
#include "spinlock.h"


static telemetry_sample_t history[TELEMETRY_HISTORY];
static uint32_t count;
static uint32_t next;

void telemetry_process(void *arg)
{
    uint64_t seen_reads = 0;
    uint64_t seen_writes = 0;
    uint64_t seen_wait = 0;
    uint64_t disk_total = 0;
    uint64_t disk_free = 0;
    uint32_t seq = 0;

    (void)arg;

    virtio_blk_stats(&seen_reads, &seen_writes, &seen_wait);
    process_telemetry(&history[0]);

    uint64_t sampled = timer_read();

    while (1)
    {
        telemetry_sample_t sample;
        uint64_t reads;
        uint64_t writes;
        uint64_t wait;

        process_sleep(TIMER_TICK_HZ);

        memset(&sample, 0, sizeof(sample));
        process_telemetry(&sample);
        virtio_blk_stats(&reads, &writes, &wait);

        knocfs_space(&disk_total, &disk_free);

        sample.seq = ++seq;
        sample.uptime = (uint32_t)(timer_ticks() / TIMER_TICK_HZ);
        uint64_t now = timer_read();
        uint64_t elapsed = now - sampled > TIMER_FREQ_HZ ? now - sampled : TIMER_FREQ_HZ;

        sampled = now;
        sample.switches = (uint32_t)((uint64_t)sample.switches * TIMER_FREQ_HZ / elapsed);
        sample.syscalls = (uint32_t)((uint64_t)sample.syscalls * TIMER_FREQ_HZ / elapsed);
        sample.denied = (uint32_t)((uint64_t)sample.denied * TIMER_FREQ_HZ / elapsed);
        sample.spawns = (uint32_t)((uint64_t)sample.spawns * TIMER_FREQ_HZ / elapsed);
        sample.top_sys = (uint32_t)((uint64_t)sample.top_sys * TIMER_FREQ_HZ / elapsed);
        sample.top_spawn = (uint32_t)((uint64_t)sample.top_spawn * TIMER_FREQ_HZ / elapsed);
        sample.disk_reads = (uint32_t)((reads - seen_reads) * TIMER_FREQ_HZ / elapsed);
        sample.disk_writes = (uint32_t)((writes - seen_writes) * TIMER_FREQ_HZ / elapsed);
        sample.disk_wait = (uint32_t)((wait - seen_wait) * TIMER_FREQ_HZ / elapsed);
        sample.ram_free_kib = page_free_count() * PAGE_SIZE / 1024;
        sample.ram_total_kib = page_total() * PAGE_SIZE / 1024;
        sample.disk_total_kib = disk_total / 1024;
        sample.disk_free_kib = disk_free / 1024;
        seen_reads = reads;
        seen_writes = writes;
        seen_wait = wait;

        uint64_t enabled = irq_save();
        history[next] = sample;
        next = (next + 1) % TELEMETRY_HISTORY;
        if (count < TELEMETRY_HISTORY)
        {
            count++;
        }
        irq_restore(enabled);
    }
}

int telemetry_get(uint32_t index, telemetry_sample_t *sample)
{
    uint64_t enabled = irq_save();

    if (index >= count)
    {
        irq_restore(enabled);
        return -1;
    }

    *sample = history[(next + TELEMETRY_HISTORY - 1 - index) % TELEMETRY_HISTORY];
    irq_restore(enabled);
    return 0;
}
