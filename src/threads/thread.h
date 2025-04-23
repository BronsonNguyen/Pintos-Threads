#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Forward declaration for donation bookkeeping */
struct lock;

/* States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING,   /* Running thread. */
  THREAD_READY,     /* Ready to run but not actually running. */
  THREAD_BLOCKED,   /* Waiting for an event to trigger. */
  THREAD_DYING      /* About to be destroyed. */
};

/* Thread identifier type. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)     /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                 /* Lowest priority. */
#define PRI_DEFAULT 31            /* Default priority. */
#define PRI_MAX 63                /* Highest priority. */

/* A kernel thread or user process. */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;                    /* Thread identifier. */
  enum thread_status status;    /* Thread state. */
  char name[16];                /* For debugging purposes. */
  uint8_t stack;               / Saved stack pointer. */
  int priority;                 /* Effective priority. */

  /* Priority-donation fields. */
  int init_priority;            /* Base priority before donations. */
  struct list donations;        /* List of threads that donated to us. */
  struct list_elem donation_elem; /* List element for donations list. */
  struct lock waiting_lock;    / Lock we’re blocked on (if any). */

  struct list_elem allelem;     /* Element in all threads list. */
  struct list_elem elem;        /* Dual-purpose list element. */

#ifdef USERPROG
  uint32_t pagedir;            / Page directory (userprog). */
#endif

  /* Owned by thread.c. */
  unsigned magic;               /* Detects stack overflow. */
};

/* If false, use round‑robin; if true, use multi‑level feedback queue. */
extern bool thread_mlfqs;

/* Thread system initialization and startup. */
void thread_init(void);
void thread_start(void);

/* Tick handling and stats. */
void thread_tick(void);
void thread_print_stats(void);

/* Thread creation and control. */
typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

/* Thread accessors. */
struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

/* Loop over all threads. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *aux);

/* Priority interface. */
int thread_get_priority(void);
void thread_set_priority(int new_priority);

/* Priority-donation helpers (used by synch.c). */
void donate_priority(struct thread *holder);
void remove_donations_for_lock(struct lock *lock);
void refresh_priority(void);

/* Nice/MLFQS stubs. */
int thread_get_nice(void);
void thread_set_nice(int nice);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

#endif /* threads/thread.h */