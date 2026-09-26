#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ulib.h"

#define THREADS 3
#define WORK 60000000UL
#define COUNT 20000

typedef struct slice
{
    unsigned long from;
    unsigned long to;
    unsigned long sum;
} slice_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static unsigned long work(unsigned long from, unsigned long to)
{
    unsigned long sum = 0;

    for (unsigned long i = from; i < to; i++)
    {
        sum += (i * i) % 7919;
    }

    return sum;
}

static void *sum_slice(void *argument)
{
    slice_t *s = argument;

    s->sum = work(s->from, s->to);
    return s;
}

static void *count_up(void *argument)
{
    (void)argument;

    for (int i = 0; i < COUNT; i++)
    {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }

    return 0;
}

static void *allocate(void *argument)
{
    long ok = 1;

    for (int i = 0; i < 2000; i++)
    {
        char *p = malloc((size_t)(16 + (i * 37) % 900));

        if (!p)
        {
            ok = 0;
            break;
        }

        memset(p, (int)(long)argument, 16);

        if (p[0] != (char)(long)argument)
        {
            ok = 0;
        }

        free(p);
    }

    return (void *)ok;
}

int main(void)
{
    pthread_t threads[THREADS];
    slice_t slices[THREADS];

    unsigned long start = uptime();
    unsigned long expected = work(0, WORK);
    unsigned long single = uptime() - start;

    start = uptime();

    for (int i = 0; i < THREADS; i++)
    {
        slices[i].from = WORK * (unsigned long)i / THREADS;
        slices[i].to = WORK * (unsigned long)(i + 1) / THREADS;

        if (pthread_create(&threads[i], 0, sum_slice, &slices[i]) != 0)
        {
            printf("threadtest: could not start a thread\n");
            return 1;
        }
    }

    unsigned long total = 0;

    for (int i = 0; i < THREADS; i++)
    {
        void *result;

        pthread_join(threads[i], &result);
        total += ((slice_t *)result)->sum;
    }

    unsigned long parallel = uptime() - start;

    printf("threadtest: %d threads summed the work: %s\n", THREADS, total == expected ? "same answer as 1 thread" : "WRONG answer");
    printf("threadtest: 1 thread %lu ms, %d threads %lu ms (%lu.%lux faster)\n", single * 10, THREADS, parallel * 10,
           parallel ? single / parallel : 0, parallel ? (single * 10 / parallel) % 10 : 0);

    for (int i = 0; i < THREADS; i++)
    {
        pthread_create(&threads[i], 0, count_up, 0);
    }

    for (int i = 0; i < THREADS; i++)
    {
        pthread_join(threads[i], 0);
    }

    printf("threadtest: mutex counter %ld (expected %d)\n", counter, THREADS * COUNT);

    long all_ok = 1;

    for (int i = 0; i < THREADS; i++)
    {
        pthread_create(&threads[i], 0, allocate, (void *)(long)(i + 1));
    }

    for (int i = 0; i < THREADS; i++)
    {
        void *ok;

        pthread_join(threads[i], &ok);
        all_ok &= (long)ok;
    }

    printf("threadtest: malloc from %d threads at once: %s\n", THREADS, all_ok ? "ok" : "FAILED");
    return total == expected && counter == THREADS * COUNT && all_ok ? 0 : 1;
}
