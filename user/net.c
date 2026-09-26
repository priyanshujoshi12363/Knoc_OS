#include <stdio.h>
#include "ulib.h"

static void print_ip(const char *label, unsigned int ip)
{
    printf("%-10s %u.%u.%u.%u\n", label, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int main(void)
{
    net_info_t info;

    if (net_info(&info) != 0 || !info.up)
    {
        printf("net: no network card (QEMU needs -netdev user,id=net0 -device virtio-net-device,netdev=net0)\n");
        return 1;
    }

    printf("net0: up\n");
    printf("%-10s %02x:%02x:%02x:%02x:%02x:%02x\n", "mac", info.mac[0], info.mac[1], info.mac[2], info.mac[3],
           info.mac[4], info.mac[5]);
    print_ip("address", info.address);
    print_ip("netmask", info.netmask);
    print_ip("gateway", info.gateway);
    print_ip("dns", info.dns);
    printf("%-10s %lu frames in, %lu frames out\n", "traffic", (unsigned long)info.frames_in,
           (unsigned long)info.frames_out);
    printf("%-10s %lu bytes in, %lu bytes out, %u open connections\n", "tcp", (unsigned long)info.bytes_in,
           (unsigned long)info.bytes_out, info.connections);
    return 0;
}
