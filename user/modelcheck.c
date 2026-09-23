#include "ulib.h"

/* An AI_AGENT program loads a model file from disk into memory, the way
   the LLM runtime will load model weights */

#define MODEL_PATH "/models/test-model.bin"
#define READ_CHUNK (1024UL * 1024)

int main(void)
{
    file_stat_t info;

    if (stat(MODEL_PATH, &info) != 0 || info.size == 0)
    {
        print("[modelcheck] " MODEL_PATH " is missing\n");
        return 1;
    }

    unsigned char *model = mem_alloc(info.size);

    if (model == 0)
    {
        print("[modelcheck] not enough memory\n");
        return 2;
    }

    int fd = open(MODEL_PATH, O_READ);
    unsigned long start = uptime();
    unsigned long loaded = 0;

    while (loaded < info.size)
    {
        long count = read(fd, model + loaded, READ_CHUNK);

        if (count <= 0)
        {
            print("[modelcheck] read failed\n");
            return 3;
        }

        loaded += (unsigned long)count;
    }

    unsigned long ticks = uptime() - start;

    close(fd);

    for (unsigned long i = 0; i < info.size; i++)
    {
        if (model[i] != (unsigned char)((i * 7 + (i >> 12)) & 0xFF))
        {
            print("[modelcheck] wrong byte in the model\n");
            return 4;
        }
    }

    print("[modelcheck] loaded ");
    print_uint(info.size / (1024 * 1024));
    print(" MiB model from " MODEL_PATH " (");
    print_uint(info.extents);
    print(" extent) in ");
    print_uint(ticks * 10);
    print(" ms, every byte verified\n");
    return 0;
}
