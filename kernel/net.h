#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "syscall_abi.h"

void net_start(const uint8_t *mac);
void net_receive(const uint8_t *frame, uint32_t length);
void net_tick(void);
void net_release(int pid);
void net_info(net_info_t *info);
int64_t net_resolve(const char *name, uint32_t *address);
int64_t net_ping(uint32_t address, uint32_t sequence);
int64_t net_connect(uint32_t address, uint32_t port);
int64_t net_send(int handle, const uint8_t *data, uint64_t length);
int64_t net_recv(int handle, uint8_t *data, uint64_t length);
int64_t net_close(int handle);

#endif
