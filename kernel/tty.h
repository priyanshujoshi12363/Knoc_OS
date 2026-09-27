#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* The terminal between the keyboard and the shell. The kernel console
   process reads the keyboard, handles the control keys itself, and
   passes everything else here while a shell owns the terminal. */

void tty_input(char c);
int64_t tty_read(char *buffer, uint64_t length);
int tty_has_input(void);
void tty_echo(char c);
void tty_echo_text(const char *text);
void tty_wait_input(uint64_t ticks);

void tty_set_owner(int pid);
int tty_has_owner(void);
void tty_set_foreground(int pid);
int tty_foreground(void);

#endif
