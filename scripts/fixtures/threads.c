#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long total;

static void *add(void *argument)
{
    long n = (long)argument;

    for (long i = 1; i <= n; i++)
    {
        pthread_mutex_lock(&lock);
        total += i;
        pthread_mutex_unlock(&lock);
    }

    return argument;
}

int main(void)
{
    pthread_t a, b;
    void *ra, *rb;

    pthread_create(&a, 0, add, (void *)1000L);
    pthread_create(&b, 0, add, (void *)2000L);
    pthread_join(a, &ra);
    pthread_join(b, &rb);
    printf("threads built by tcc: total %ld, joined %ld and %ld\n", total, (long)ra, (long)rb);
    return 0;
}
