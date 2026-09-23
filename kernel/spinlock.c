#include "spinlock.h"
#include "trap.h"

/* A lock taken by one core is waited on by the other. The owner also
   turns its interrupts off, so a timer tick can't switch to another
   process that would then wait forever for the same lock. */

uint64_t spin_lock(spinlock_t *lock)
{
    uint64_t previous;

    asm volatile("csrrc %0, sstatus, %1"
                 : "=r"(previous)
                 : "r"((uint64_t)SSTATUS_SIE));

    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0)
    {
    }

    return previous & SSTATUS_SIE;
}

void spin_unlock(spinlock_t *lock, uint64_t interrupts)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);

    if (interrupts)
    {
        asm volatile("csrs sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));
    }
}

int spin_trylock(spinlock_t *lock)
{
    return __atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

uint64_t irq_save(void)
{
    uint64_t previous;

    asm volatile("csrrc %0, sstatus, %1"
                 : "=r"(previous)
                 : "r"((uint64_t)SSTATUS_SIE));

    return previous & SSTATUS_SIE;
}

void irq_restore(uint64_t interrupts)
{
    if (interrupts)
    {
        asm volatile("csrs sstatus, %0" :: "r"((uint64_t)SSTATUS_SIE));
    }
}

int spin_is_locked(spinlock_t *lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}
