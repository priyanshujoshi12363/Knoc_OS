#ifndef GUARDIAN_H
#define GUARDIAN_H

#include <stdint.h>
#include "device.h"

#define GUARDIAN_HEALTHY_TICKS 6000

int guardian_init(void);
void guardian_heartbeat(void);
void guardian_set_current(int pid, const char *name);
void guardian_start_watch(void);
void guardian_set_safe_mode(int enabled);

void guardian_record_trap(uint64_t scause,
                          uint64_t sepc,
                          uint64_t stval,
                          uint64_t ra,
                          uint64_t sp);
void guardian_report_panic(const char *message);

uint64_t guardian_boot_report(device_t *disk);
void guardian_healthy_process(void *arg);

#endif
