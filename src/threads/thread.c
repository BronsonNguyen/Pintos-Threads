/* threads/thread.c */
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
#include <list.h>
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define THREAD_MAGIC 0xcd6abf4b
#define TIME_SLICE 4

/* Statistics. */
static long long idle_ticks;
static long long kernel_ticks;
static long long user_ticks;
static unsigned thread_ticks;

/* Scheduling lists. */
static struct list ready_list;
static struct list all_list;

/* Idle and initial threads. */
static struct thread *idle_thread;
static struct thread *initial_thread;

/* Lock for TID allocation. */
static struct lock tid_lock;

/* MLFQS flag. */
bool thread_mlfqs = false;

/* Frame for starting a kernel thread. */
struct kernel_thread_frame {
  void *eip;
  thread_func *function;
  void *aux;
};

/* Comparator for ready_list by thread.elem. */
static bool thread_priority_cmp(const struct list_elem *a,
                                const struct list_elem *b,
                                void *aux UNUSED) {
  struct thread *t1 = list_entry(a, struct thread, elem);
  struct thread *t2 = list_entry(b, struct thread, elem);
  return t1->priority > t2->priority;
}

/* Comparator for donations list by thread.donation_elem. */
static bool donation_priority_cmp(const struct list_elem *a,
                                  const struct list_elem *b,
                                  void *aux UNUSED) {
  struct thread *t1 = list_entry(a, struct thread, donation_elem);
  struct thread *t2 = list_entry(b, struct thread, donation_elem);
  return t1->priority > t2->priority;
}

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

void donate_priority(struct thread *holder);
void remove_donations_for_lock(struct lock *lock);
void refresh_priority(void);

struct thread *thread_current(void) {
  struct thread *t = running_thread();
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);
  return t;
}

tid_t thread_tid(void) {
  return thread_current()->tid;
}

const char *thread_name(void) {
  return thread_current()->name;
}

void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);

  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();

  /* Add the initial thread to the all_list. */
  list_push_back(&all_list, &initial_thread->allelem);
}

void thread_start(void) {
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  intr_enable();
  sema_down(&idle_started);
}

void thread_tick(void) {
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

void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
         idle_ticks, kernel_ticks, user_ticks);
}

tid_t thread_create(const char *name, int priority,
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

  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  thread_unblock(t);
  return tid;
}

void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);
  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

void thread_unblock(struct thread *t) {
  enum intr_level old = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem, thread_priority_cmp, NULL);
  t->status = THREAD_READY;
  intr_set_level(old);
}

void thread_yield(void) {
  struct thread *cur = thread_current();
  enum intr_level old = intr_disable();
  if (cur != idle_thread)
    list_insert_ordered(&ready_list, &cur->elem, thread_priority_cmp, NULL);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old);
}

void thread_foreach(thread_action_func *func, void *aux) {
  ASSERT(intr_get_level() == INTR_OFF);
  for (struct list_elem *e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
    func(list_entry(e, struct thread, allelem), aux);
}

int thread_get_priority(void) {
  return thread_current()->priority;
}

void thread_set_priority(int new_priority) {
  struct thread *cur = thread_current();
  cur->init_priority = new_priority;
  refresh_priority();
  if (!list_empty(&ready_list)) {
    struct thread *t = list_entry(list_front(&ready_list), struct thread, elem);
    if (t->priority > cur->priority)
      thread_yield();
  }
}

void thread_set_nice(int nice UNUSED) {}
int thread_get_nice(void) { return 0; }
int thread_get_load_avg(void) { return 0; }
int thread_get_recent_cpu(void) { return 0; }

void thread_exit(void) {
  ASSERT(!intr_context());
#ifdef USERPROG
  process_exit();
#endif
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

static void
init_thread(struct thread *t, const char *name, int priority) {
  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t *)t + PGSIZE;  // Ensure stack points to the top of the page
  t->init_priority = priority;
  t->priority = priority;
  list_init(&t->donations);
  t->waiting_lock = NULL;
  t->magic = THREAD_MAGIC;

  enum intr_level old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);
}

static struct thread *next_thread_to_run(void) {
  if (list_empty(&ready_list))
    return idle_thread;
  return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

void thread_schedule_tail(struct thread *prev) {
  struct thread *cur = running_thread();
  ASSERT(intr_get_level() == INTR_OFF);

  cur->status = THREAD_RUNNING;
  thread_ticks = 0;
#ifdef USERPROG
  process_activate();
#endif
  if (prev && prev->status == THREAD_DYING && prev != initial_thread)
    palloc_free_page(prev);
}

static void schedule(void) {
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

static struct thread *running_thread(void) {
  uint32_t *esp;
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

static bool is_thread(struct thread *t) {
  return t && t->magic == THREAD_MAGIC;
}

static void *alloc_frame(struct thread *t, size_t size) {
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);
  t->stack -= size;
  return t->stack;
}

static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;
  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);
  return tid;
}

static void idle(void *idle_started_ UNUSED) {
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);
  for (;;) {
    intr_disable();
    thread_block();
    asm volatile("sti; hlt" : : : "memory");
  }
}

static void kernel_thread(thread_func *function, void *aux) {
  ASSERT(function != NULL);
  intr_enable();
  function(aux);
  thread_exit();
}

void
donate_priority(struct thread *holder) {
  struct thread *cur = thread_current();
  int depth = 0;

  while(holder != NULL && depth < 8) {
    list_insert_ordered(&holder->donations, &cur->donation_elem, donation_priority_cmp, NULL);
    if(cur->priority > holder->priority) {
      holder->priority = cur->priority;
    }

    if(depth == 0) {
      list_remove(&cur->donation_elem);
      list_insert_ordered(&holder->donations, &cur->donation_elem, donation_priority_cmp, NULL);
    }

    cur = holder;
    holder = holder->waiting_lock ? holder->waiting_lock->holder : NULL;
    depth++;

  }  
}


void remove_donations_for_lock(struct lock *lock) {
  struct thread *cur = thread_current();
  struct list_elem *e = list_begin(&cur->donations);

  while (e != list_end(&cur->donations)) {
    struct thread *t = list_entry(e, struct thread, donation_elem);
    if (t->waiting_lock == lock)
      e = list_remove(e);
    else
      e = list_next(e);
  }

  refresh_priority();
}

void refresh_priority(void) {
  struct thread *cur = thread_current();
  int old_priority = cur->priority;
  int new_priority = cur->init_priority;

  if (!list_empty(&cur->donations)) {
    struct thread *highest = list_entry(list_front(&cur->donations), 
                                      struct thread, donation_elem);
    new_priority = (new_priority > highest->priority) ? new_priority : highest->priority;
  }
  cur->priority = new_priority;

  if (new_priority < old_priority && !list_empty(&ready_list)) {
    thread_yield();
  }
}

uint32_t thread_stack_ofs = offsetof(struct thread, stack);