#ifndef UART_H
#define UART_H

#include <stdint.h>

#define UART_BASE 0x10000000UL
#define UART_IRQ 10

void uart_putc(char c);
void uart_puts(const char *str);
void uart_put_hex(uint64_t value);
void uart_put_uint(uint64_t value);

void uart_register(void);

#endif
