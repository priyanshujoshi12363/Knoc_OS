#include <stdio.h>
#include <stdlib.h>
#include "ulib.h"

int main(int argc, char **argv)
{
    unsigned int ip;
    int count = argc > 2 ? atoi(argv[2]) : 4;
    int received = 0;
    long total = 0;

    if (argc < 2)
    {
        printf("usage: ping HOST [COUNT]\n");
        return 1;
    }

    int result = net_resolve(argv[1], &ip);

    if (result != 0)
    {
        printf("ping: cannot find %s (%s)\n", argv[1], result == E_TIMEOUT ? "no answer from DNS" : "unknown host");
        return 1;
    }

    printf("PING %s (%u.%u.%u.%u)\n", argv[1], ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);

    for (int i = 1; i <= count; i++)
    {
        long ms = net_ping(ip, (unsigned int)i);

        if (ms >= 0)
        {
            printf("reply from %u.%u.%u.%u: seq=%d time=%ld ms\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255,
                   ip & 255, i, ms);
            received++;
            total += ms;
        }
        else
        {
            printf("seq=%d: no reply\n", i);
        }

        if (i < count)
        {
            sleep(50);
        }
    }

    printf("%d sent, %d received", count, received);

    if (received)
    {
        printf(", average %ld ms", total / received);
    }

    printf("\n");
    return received ? 0 : 1;
}
