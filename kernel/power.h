#ifndef POWER_H
#define POWER_H

#define POWER_BASE 0x00100000UL

#define POWER_COMMAND_OFF 0
#define POWER_COMMAND_REBOOT 1

void power_off(void);
void power_reboot(void);
void power_register(void);

#endif
