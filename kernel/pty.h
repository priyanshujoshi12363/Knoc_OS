#ifndef PTY_H
#define PTY_H

#include <stdint.h>

int pty_open(int owner);
int pty_owned(int id, int owner);
void pty_close(int id);
void pty_release_owner(int owner);
int64_t pty_output(int id, const char *data, uint64_t length);
int64_t pty_take_output(int id, char *out, uint64_t length);
int64_t pty_give_input(int id, const char *data, uint64_t length);
int64_t pty_read_input(int id, char *out, uint64_t length);
int pty_has_input(int id);
void pty_wait_input(int id, uint64_t ticks);
void pty_set_foreground(int id, int pid);

#endif
