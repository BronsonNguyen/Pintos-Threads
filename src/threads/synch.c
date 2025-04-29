/* threads/synch.c
 *
 * Synchronization primitives with strict priority scheduling
 * and priority donation (Project 1).
 */

#include "threads/synch.h"
#include <debug.h>
#include <stddef.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Comparator for sema->waiters: highest‐priority thread first. */
static bool
sema_priority_cmp(const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED) 
{
  struct thread *t1 = list_entry(a, struct thread, elem);
  struct thread *t2 = list_entry(b, struct thread, elem);
  return t1->priority > t2->priority;
}

void
sema_init(struct semaphore *sema, unsigned value) 
{
  ASSERT(sema != NULL);
  sema->value = value;
  list_init(&sema->waiters);
}

void
sema_down(struct semaphore *sema) 
{
  enum intr_level old = intr_disable();
  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  while (sema->value == 0) 
  {
    /* Block current thread, ordered by priority */
    list_insert_ordered(&sema->waiters, &thread_current()->elem,
                        sema_priority_cmp, NULL);
    thread_block();
  }
  sema->value--;
  intr_set_level(old);
}

bool
sema_try_down(struct semaphore *sema) 
{
  enum intr_level old = intr_disable();
  bool success = false;
  ASSERT(sema != NULL);

  if (sema->value > 0) 
  {
    sema->value--;
    success = true;
  }
  intr_set_level(old);
  return success;
}

void
sema_up(struct semaphore *sema) 
{
  enum intr_level old = intr_disable();
  ASSERT(sema != NULL);

  if (!list_empty(&sema->waiters)) 
  {
    /* Wake up highest‐priority waiter */
    list_sort(&sema->waiters, sema_priority_cmp, NULL);
    struct thread *t = 
      list_entry(list_pop_front(&sema->waiters),
                 struct thread, elem);
    thread_unblock(t);
  }
  sema->value++;
  intr_set_level(old);

  /* If we just unblocked someone higher-priority than us, yield. */
  if (!intr_context())
    thread_yield();
}

/* One semaphore in a list. */
struct semaphore_elem 
{
  struct list_elem elem;      /* List element. */
  struct semaphore semaphore; /* This semaphore. */
};

/* Lock implementation (built on semaphores). */

void
lock_init(struct lock *lock) 
{
  ASSERT(lock != NULL);
  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

void
lock_acquire(struct lock *lock) 
{
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  /* Priority donation handled in thread code elsewhere */
  sema_down(&lock->semaphore);
  lock->holder = thread_current();
}

bool
lock_try_acquire(struct lock *lock) 
{
  bool success;
  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success)
    lock->holder = thread_current();
  return success;
}

void
lock_release(struct lock *lock) 
{
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock));

  lock->holder = NULL;
  sema_up(&lock->semaphore);
}

bool
lock_held_by_current_thread(const struct lock *lock) 
{
  ASSERT(lock != NULL);
  return lock->holder == thread_current();
}

/* Condition variables. */

void
cond_init(struct condition *cond) 
{
  ASSERT(cond != NULL);
  list_init(&cond->waiters);
}

void
cond_wait(struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  list_insert_ordered(&cond->waiters, &waiter.elem,
                      sema_priority_cmp, NULL);

  lock_release(lock);
  sema_down(&waiter.semaphore);
  lock_acquire(lock);
}

void
cond_signal(struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters)) 
  {
    /* Wake the highest‐priority waiter */
    list_sort(&cond->waiters, sema_priority_cmp, NULL);
    struct semaphore_elem *se = 
      list_entry(list_pop_front(&cond->waiters),
                 struct semaphore_elem, elem);
    sema_up(&se->semaphore);
  }
}

void
cond_broadcast(struct condition *cond, struct lock *lock) 
{
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters))
    cond_signal(cond, lock);
}

