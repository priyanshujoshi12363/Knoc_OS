#include <stdlib.h>

#define ARGV_MAX 32

long getargs(char *buffer, unsigned long length);
int main(int argc, char **argv);

__attribute__((section(".text.start"))) void _start(void)
{
    static char args[256];
    static char name[] = "program";
    static char *argv[ARGV_MAX + 1];
    int argc = 0;

    argv[argc++] = name;
    getargs(args, sizeof(args));

    for (char *p = args; *p && argc < ARGV_MAX;)
    {
        while (*p == ' ')
        {
            *p++ = 0;
        }

        if (!*p)
        {
            break;
        }

        argv[argc++] = p;

        while (*p && *p != ' ')
        {
            p++;
        }
    }

    argv[argc] = NULL;
    exit(main(argc, argv));
}
