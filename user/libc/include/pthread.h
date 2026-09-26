#ifndef _KNOC_PTHREAD_H
#define _KNOC_PTHREAD_H

#include <stddef.h>

typedef int pthread_t;
typedef int pthread_attr_t;

typedef struct
{
    int locked;
} pthread_mutex_t;

typedef int pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
pthread_t pthread_self(void);
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int sched_yield(void);

#endif
