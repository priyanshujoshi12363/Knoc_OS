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
        sample.disk_reads = (uint32_t)(reads - seen_reads);
        sample.disk_writes = (uint32_t)(writes - seen_writes);
        sample.disk_wait = (uint32_t)(wait - seen_wait);
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
