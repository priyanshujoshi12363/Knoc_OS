#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include "../../kernel/syscall_abi.h"

#define THREAD_STACK (256 * 1024)
#define RESULTS_MAX 64

typedef struct start
{
    void *(*function)(void *);
    void *argument;
    int slot;
} start_t;

typedef struct result
{
    int used;
    pthread_t thread;
    void *value;
    void *stack;
} result_t;

long syscall(long number, long a0, long a1, long a2);
int thread_spawn(void (*function)(void *), void *argument, void *stack, unsigned long stack_size);
long wait(int pid);

static result_t results[RESULTS_MAX];
static pthread_mutex_t results_lock = PTHREAD_MUTEX_INITIALIZER;

static void run(void *context)
{
    start_t start = *(start_t *)context;

    free(context);
    results[start.slot].value = start.function(start.argument);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*function)(void *), void *argument)
{
    (void)attr;

    int slot = -1;

    pthread_mutex_lock(&results_lock);

    for (int i = 0; i < RESULTS_MAX; i++)
    {
        if (!results[i].used)
        {
            results[i].used = 1;
            slot = i;
            break;
        }
    }

    pthread_mutex_unlock(&results_lock);

    if (slot < 0)
    {
        return EAGAIN;
    }

    start_t *start = malloc(sizeof(start_t));
    void *stack = malloc(THREAD_STACK);

    if (!start || !stack)
    {
        free(start);
        free(stack);
        results[slot].used = 0;
        return EAGAIN;
    }

    start->function = function;
    start->argument = argument;
    start->slot = slot;
    results[slot].value = 0;
    results[slot].stack = stack;

    int id = thread_spawn(run, start, stack, THREAD_STACK);

    if (id < 0)
    {
        free(start);
        free(stack);
        results[slot].used = 0;
        return EAGAIN;
    }

    results[slot].thread = id;
    *thread = id;
    return 0;
}

int pthread_join(pthread_t thread, void **result)
{
    int slot = -1;

    for (int i = 0; i < RESULTS_MAX; i++)
    {
        if (results[i].used && results[i].thread == thread)
        {
            slot = i;
        }
    }

    if (slot < 0)
    {
        return ESRCH;
    }

    wait(thread);

    if (result)
    {
        *result = results[slot].value;
    }

    free(results[slot].stack);
    results[slot].used = 0;
    return 0;
}

pthread_t pthread_self(void)
{
    return (pthread_t)syscall(SYS_GETPID, 0, 0, 0);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    mutex->locked = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    return atomic_exchange_explicit(&mutex->locked, 1, memory_order_acquire) ? EBUSY : 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int spins = 0;

    while (atomic_exchange_explicit(&mutex->locked, 1, memory_order_acquire))
    {
        if (++spins % 1000 == 0)
        {
            sched_yield();
        }
    }

    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    atomic_store_explicit(&mutex->locked, 0, memory_order_release);
    return 0;
}

int sched_yield(void)
{
    syscall(SYS_YIELD, 0, 0, 0);
    return 0;
}
