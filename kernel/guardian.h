#ifndef GUARDIAN_H
#define GUARDIAN_H

#include <stdint.h>
#include "device.h"

#define GUARDIAN_HEALTHY_TICKS 6000
#define GUARDIAN_POLL_TICKS 10
#define GUARDIAN_VERDICT_TIMEOUT 100
#define GUARDIAN_RESTART_LIMIT 3

int guardian_init(void);
void guardian_heartbeat(void);
void guardian_set_current(int pid, const char *name);
void guardian_start_watch(void);
void guardian_set_safe_mode(int enabled);
int guardian_restart_safe_mode(void);
int guardian_driver_disabled(const char *name);

void guardian_record_trap(uint64_t scause,
                          uint64_t sepc,
                          uint64_t stval,
                          uint64_t ra,
                          uint64_t sp);
void guardian_report_panic(const char *message);

uint64_t guardian_boot_report(device_t *disk);
void guardian_process(void *arg);

#endif
