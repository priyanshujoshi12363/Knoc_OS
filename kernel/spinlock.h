#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct spinlock
{
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

uint64_t spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock, uint64_t interrupts);
int spin_trylock(spinlock_t *lock);
int spin_is_locked(spinlock_t *lock);

uint64_t irq_save(void);
void irq_restore(uint64_t interrupts);

#endif
