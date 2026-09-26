#include "ulib.h"

int main(void)
{
    char args[ARGS_MAX];

    getargs(args, sizeof(args));

    if (strcmp(args, "repeat") == 0)
    {
        print("[noperm] retrying a file it has no permission to read\n");

        for (int i = 0; i < 40; i++)
        {
            open("/hello.txt", O_READ);
            sleep(25);
        }

        return 0;
    }

    int result = spawn("hello");

    if (result != E_PERM)
    {
        print("[noperm] spawn without the SPAWN capability was not refused\n");
        return 1;
    }

    if (open("/hello.txt", O_READ) != E_PERM)
    {
        print("[noperm] open without the FILES_READ capability was not refused\n");
        return 2;
    }

    print("[noperm] spawn and open were refused: this program has no SPAWN or FILES capability\n");
    return 0;
}
