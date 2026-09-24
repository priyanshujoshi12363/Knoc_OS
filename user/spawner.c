#include "ulib.h"

int main(void)
{
    while (1)
    {
        int pid = spawn("quiet");

        if (pid > 0)
        {
            wait(pid);
        }

        sleep(5);
    }
}
