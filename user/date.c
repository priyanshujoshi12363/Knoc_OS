#include <stdio.h>
#include <time.h>
#include "ulib.h"

int main(void)
{
    char text[64];
    long seconds = realtime();

    if (seconds <= 0)
    {
        printf("date: this machine has no real-time clock\n");
        return 1;
    }

    time_t now = (time_t)seconds;

    strftime(text, sizeof(text), "%a %b %d %H:%M:%S UTC %Y", gmtime(&now));
    printf("%s\n", text);
    return 0;
}
