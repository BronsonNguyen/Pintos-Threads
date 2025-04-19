#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include <list.h>  /* for list_insert_ordered */
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member. */
#define THREAD_MAGIC 0xcd6abf4b

/* Scheduling. */
#define TIME_SLICE 4

static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;
static unsigned thread_ticks;

static struct list ready_list;   /* Ready-to-run threads, ordered */
static struct list all_list;     /* All threads */
static struct thread *idle_thread;
static struct thread *initial_thread;
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void *eip;
  thread_func *function;
  void *aux;
};

/* Forward declarations. */
static void kernel_thread(thread_func *, void *aux);
static void idle(void *aux UNUSED);
static struct thread *running_thread(void);
static bool is_thread(struct thread *);
static void init_thread(struct thread *, const char *name, int priority);
static void *alloc_frame(struct thread *, size_t size);
static struct thread *next_thread_to_run(void);
static void schedule(void);
void thread_schedule_tail(struct thread *prev);
static tid_t allocate_tid(void);

/* Priority comparator: higher priority first */
static bool
thread_priority_cmp(const struct list_elem *a,
                    const struct list_elem *b,
                    void *aux UNUSED)
{
  struct thread *t1 = list_entry(a, struct thread, elem);
  struct thread *t2 = list_entry(b, struct thread, elem);
  return t1->priority > t2->priority;
}

/* Initializes the threading system. */
void
thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);

  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling and the idle thread. */
void
thread_start(void) {
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  intr_enable();
  sema_down(&idle_started);
}

/* Called by timer interrupt handler at each tick. */
void
thread_tick(void) {
  struct thread *t = thread_current();
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void
thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with given PRIORITY. */
tid_t
thread_create(const char *name, int priority,
              thread_func *function, void *aux) {
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT(function != NULL);

  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  thread_unblock(t);
  return tid;
}

/* Puts the current thread to sleep. */
void
thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Transitions T from blocked to ready, inserting by priority. */
void
thread_unblock(struct thread *t) {
  enum intr_level old = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem,
                      thread_priority_cmp, NULL);
  t->status = THREAD_READY;
  intr_set_level(old);
}

/* Yields the CPU, re-inserting current thread by priority. */
void
thread_yield(void) {
  struct thread *cur = thread_current();
  enum intr_level old = intr_disable();
  if (cur != idle_thread)
    list_insert_ordered(&ready_list, &cur->elem,
                        thread_priority_cmp, NULL);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old);
}

/* Invoke FUNC on all threads. */
void
thread_foreach(thread_action_func *func, void *aux) {
  ASSERT(intr_get_level() == INTR_OFF);
  for (struct list_elem *e = list_begin(&all_list);
       e != list_end(&all_list);
       e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority(int new_priority) {
  struct thread *cur = thread_current();
  cur->priority = new_priority;
  /* Preempt if a higher-priority thread is waiting */
  if (!list_empty(&ready_list)) {
    struct thread *t = list_entry(list_front(&ready_list),
                                  struct thread, elem);
    if (t->priority > cur->priority)
      thread_yield();
  }
}

/* Returns the current thread's priority. */
int
thread_get_priority(void) {
  return thread_current()->priority;
}

/* Initialize T as a blocked thread named NAME. */
static void
init_thread(struct thread *t, const char *name, int priority) {
  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t *)t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  enum intr_level old = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old);
}

/* Chooses and returns the next thread to run. */
static struct thread *
next_thread_to_run(void) {
  if (list_empty(&ready_list))
    return idle_thread;
  else
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Completes a thread switch. */
void
thread_schedule_tail(struct thread *prev) {
  struct thread *cur = running_thread();
  ASSERT(intr_get_level() == INTR_OFF);

  cur->status = THREAD_RUNNING;
  thread_ticks = 0;
#ifdef USERPROG
  process_activate();
#endif

  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
    palloc_free_page(prev);
}

/* Schedules a new process. */
static void
schedule(void) {
  struct thread *cur = running_thread();
  struct thread *next = next_thread_to_run();
  struct thread *prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_schedule_tail(prev);
}

/* Returns the running thread. */
struct thread *
running_thread(void) {
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread(struct thread *t) {
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Allocates a SIZE-byte frame at top of T's stack. */
static void *
alloc_frame(struct thread *t, size_t size) {
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Idle thread: runs when no one else is ready. */
static void
idle(void *idle_started_ UNUSED) {
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    intr_disable();
    thread_block();
    asm volatile ("sti; hlt" : : : "memory");
  }
}

/* Kernel thread function wrapper. */
static void
kernel_thread(thread_func *function, void *aux) {
  ASSERT(function != NULL);
  intr_enable();
  function(aux);
  thread_exit();
}

/* Offset of `stack' member within `struct thread' for switch.S */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);
