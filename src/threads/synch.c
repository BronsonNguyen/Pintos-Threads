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
    list_sort(&sema->waiters, sema_priority_cmp, NULL);
    struct thread *t = list_entry(list_pop_front(&sema->waiters), struct thread, elem);

    if (thread_mlfqs)
      thread_update_priority(t);

    thread_unblock(t);

    /* Yield if the unblocked thread is higher priority */
    if (!intr_context() && t->priority > thread_current()->priority)
      thread_yield();
  }

  sema->value++;
  intr_set_level(old);
}
/* One semaphore in a list. */
struct semaphore_elem 
{
  struct list_elem elem;      /* List element. */
  int *priority;
  struct semaphore semaphore; /* This semaphore. */
};

/* Compare two semaphore_elems based on the priority of the first thread in their waiters list */
static bool
cond_sema_priority_cmp(const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED)
{
  struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
  struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

  return sa->priority > sb->priority;
}

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

  struct thread *cur = thread_current();

  if (lock->holder != NULL) {
    cur->waiting_for = lock;
    struct thread *holder = lock->holder;

    while (holder != NULL && cur->priority > holder->priority) {
      holder->priority = cur->priority;

      if (!cur->donated) {
        list_push_back(&holder->donations, &cur->donation_elem);
        cur->donated = true;
      }

      if (holder->waiting_for != NULL)
        holder = holder->waiting_for->holder;
      else
        break;
    }
  }

  sema_down(&lock->semaphore);

  cur->waiting_for = NULL;
  lock->holder = cur;

  thread_yield();
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

  struct thread *cur = thread_current();
  lock->holder = NULL;

  struct list_elem *e = list_begin(&cur->donations);
  while (e != list_end(&cur->donations)) {
    struct thread *t = list_entry(e, struct thread, donation_elem);
    struct list_elem *next = list_next(e);

    if (t->waiting_for == lock) {
      list_remove(&t->donation_elem);
      t->donated = false;
    }

    e = next;
  }

  /* Recalculate current priority */
  int max_priority = cur->original_priority;
  for (e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, donation_elem);
    if (t->priority > max_priority)
      max_priority = t->priority;
  }

  cur->priority = max_priority;

  sema_up(&lock->semaphore);
  thread_yield();
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
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  struct semaphore_elem waiter;
  sema_init(&waiter.semaphore, 0);
  waiter.priority = thread_current()->priority;
  /* Insert in sorted order (by priority of the waiting thread) */
  list_insert_ordered(&cond->waiters, &waiter.elem, cond_sema_priority_cmp, NULL);

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

