#include "ulib.h"

static void field(unsigned long value)
{
    print_uint(value);
    print(",");
}

int main(void)
{
    telemetry_sample_t sample;
    unsigned int last = 0;

    while (1)
    {
        if (telemetry(0, &sample) == 0 && sample.seq != last)
        {
            last = sample.seq;
            print("[T] ");
            field(sample.seq);
            field(sample.cpu_busy);
            field(sample.switches);
            field(sample.syscalls);
            field(sample.denied);
            field(sample.processes);
            field(sample.spawns);
            field(sample.crashes);
            field(sample.disk_reads);
            field(sample.disk_writes);
            field(sample.disk_wait);
            field(sample.ram_free_kib);
            field(sample.ram_total_kib);
            field(sample.user_memory_kib);
            field(sample.disk_free_kib);
            field(sample.disk_total_kib);
            field(sample.top_cpu);
            field(sample.top_mem_kib);
            field(sample.top_sys);
            field(sample.top_spawn);
            print(sample.top_cpu_name);
            print(",");
            print(sample.top_mem_name);
            print(",");
            print(sample.top_sys_name);
            print(",");
            print(sample.top_spawn_name);
            print("\n");
        }

        sleep(20);
    }
}
