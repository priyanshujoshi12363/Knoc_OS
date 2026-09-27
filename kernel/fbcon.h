#ifndef FBCON_H
#define FBCON_H

#include <stdint.h>

void fbcon_init(void);
void fbcon_init_splash(void);
void fbcon_reveal(void);
void fbcon_putc(char c);
void fbcon_tick(void);
void fbcon_pause(void);
void fbcon_resume(void);
int fbcon_active(void);
uint32_t fbcon_columns(void);
uint32_t fbcon_rows(void);

#endif
