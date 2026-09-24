#include "ulib.h"

#define PATH "/tmp/filler.bin"
#define CHUNK 4096

static unsigned char buffer[CHUNK];

int main(void)
{
    char args[ARGS_MAX];
    long limit;

    getargs(args, sizeof(args));
    limit = parse_number(args);

    if (limit <= 0)
    {
        limit = 24;
    }

    memset(buffer, 3, sizeof(buffer));

    int fd = open(PATH, O_WRITE | O_CREATE | O_TRUNC);
    unsigned long written = 0;

    while (fd >= 0 && written < (unsigned long)limit * 1024 * 1024)
    {
        for (int i = 0; i < 64; i++)
        {
            write(fd, buffer, CHUNK);
        }

        written += 64 * CHUNK;
        sleep(50);
    }

    close(fd);

    while (1)
    {
        sleep(1000);
    }
}
