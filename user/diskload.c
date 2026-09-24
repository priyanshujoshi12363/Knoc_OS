#include "ulib.h"

#define PATH "/tmp/diskload.bin"
#define CHUNK 4096
#define CHUNKS 64

static unsigned char buffer[CHUNK];

int main(void)
{
    memset(buffer, 7, sizeof(buffer));

    while (1)
    {
        int fd = open(PATH, O_READ | O_WRITE | O_CREATE | O_TRUNC);

        for (int i = 0; i < CHUNKS && fd >= 0; i++)
        {
            write(fd, buffer, CHUNK);
        }

        seek(fd, 0);

        for (int i = 0; i < CHUNKS && fd >= 0; i++)
        {
            read(fd, buffer, CHUNK);
        }

        close(fd);
    }
}
