/* threads/synch.c
 *
 * Synchronisation primitives with strict priority scheduling and
 * priority donation (Project 1).
 */

#include "threads/synch.h"
#include <debug.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* -----------------------  Local helpers  ------------------------- */

/* For lists of struct thread – highest priority first. */
static bool
sema_priority_cmp(const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED) {
  const struct thread *t1 = list_entry(a, struct thread, elem);
  const struct thread *t2 = list_entry(b, struct thread, elem);
  return t1->priority > t2->priority;
}

/* For lists of struct semaphore_elem – highest waiter priority first. */
static bool
condvar_sema_priority_cmp(const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux UNUSED) {
  const struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
  const struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);
  return sa->thread->priority > sb->thread->priority;
}

/* -----------------------  Semaphores  ---------------------------- */

void
sema_init(struct semaphore *sema, unsigned value) {
  ASSERT(sema != NULL);
  sema->value = value;
  list_init(&sema->waiters);
}

void
sema_down(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  while (sema->value == 0) {
    /* Insert current thread by priority (not FIFO). */
    list_insert_ordered(&sema->waiters,
                        &thread_current()->elem,
                        sema_priority_cmp, NULL);
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}

bool
sema_try_down(struct semaphore *sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  success = sema->value > 0;
  if (success) {
    sema->value--;
  }
  intr_set_level(old_level);

  return success;
}

void
sema_up(struct semaphore *sema) {
  enum intr_level old_level;
  struct thread *next = NULL;

  ASSERT(sema != NULL);

  old_level = intr_disable();

  /* Wake the highest-priority waiter, if any. */
  if (!list_empty(&sema->waiters)) {
    list_sort(&sema->waiters, sema_priority_cmp, NULL);
    next = list_entry(list_pop_front(&sema->waiters),
                      struct thread, elem);
    thread_unblock(next);
  }
  sema->value++;

  /* Pre-empt if the awakened thread has a higher priority. */
  if (next != NULL &&
      next->priority > thread_current()->priority &&
      !intr_context()) {
    thread_yield();
  }

  intr_set_level(old_level);
}

/* -----------------------  Locks  --------------------------------- */

void
lock_init(struct lock *lock) {
  ASSERT(lock != NULL);
  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

void
lock_acquire(struct lock *lock) {
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  /* Priority donation chain (project 1). */
  if (lock->holder != NULL && !thread_mlfqs) {
    struct thread *cur = thread_current();
    cur->waiting_lock = lock;
    donate_priority(lock->holder);
  }

  sema_down(&lock->semaphore);
  thread_current()->waiting_lock = NULL;
  lock->holder = thread_current();
}

bool
lock_try_acquire(struct lock *lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success) {
    lock->holder = thread_current();
  }
  return success;
}

void
lock_release(struct lock *lock) {
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock));

  lock->holder = NULL;

  if (!thread_mlfqs) {
    remove_donations_for_lock(lock);
    refresh_priority();
  }

  sema_up(&lock->semaphore);
}

bool
lock_held_by_current_thread(const struct lock *lock) {
  ASSERT(lock != NULL);
  return lock->holder == thread_current();
}

/* -----------------------  Condition variables  ------------------- */

void
cond_init(struct condition *cond) {
  ASSERT(cond != NULL);
  list_init(&cond->waiters);
}

void
cond_wait(struct condition *cond, struct lock *lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  waiter.thread = thread_current();

  list_insert_ordered(&cond->waiters, &waiter.elem,
                      condvar_sema_priority_cmp, NULL);

  thread_current()->waiting_lock = lock;  // Set waiting lock
  lock_release(lock);
  sema_down(&waiter.semaphore);
  thread_current()->waiting_lock = NULL;  // Clear waiting lock
  lock_acquire(lock);
}

void
cond_signal(struct condition *cond, struct lock *lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters)) {
    list_sort(&cond->waiters, condvar_sema_priority_cmp, NULL);
    struct semaphore_elem *se = list_entry(list_pop_front(&cond->waiters),
                                           struct semaphore_elem, elem);
    sema_up(&se->semaphore);
  }
}

void
cond_broadcast(struct condition *cond, struct lock *lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  while (!list_empty(&cond->waiters)) {
    cond_signal(cond, lock);
  }
}