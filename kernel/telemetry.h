#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include "syscall_abi.h"

#define TELEMETRY_HISTORY 600

void telemetry_process(void *arg);
int telemetry_get(uint32_t index, telemetry_sample_t *sample);

#endif
