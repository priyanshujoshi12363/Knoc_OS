#include "ulib.h"

#define RESTARTS_MAX 3
#define CALM_TICKS 6000

int main(void)
{
    int restarts = 0;

    while (1)
    {
        unsigned long started = uptime();
        int pid = spawn("desktop");

        if (pid < 0)
        {
            print("[SESSION] the desktop could not start: the text console stays on the screen\n");
            return 1;
        }

        long code = wait(pid);

        if (code != E_CRASHED && code != E_KILLED)
        {
            print("[SESSION] the desktop closed\n");
            return 0;
        }

        if (uptime() - started > CALM_TICKS)
        {
            restarts = 0;
        }

        if (++restarts > RESTARTS_MAX)
        {
            print("[SESSION] the desktop crashed again: the text console stays on the screen\n");
            return 1;
        }

        print("[SESSION] the desktop crashed: starting it again (");
        print_uint((unsigned long)restarts);
        print(" of 3)\n");
    }
}
