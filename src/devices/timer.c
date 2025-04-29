/* devices/timer.c */
#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <list.h>

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* List of sleeping threads, ordered by wakeup_time (earliest first). */
static struct list sleep_list;

/* Number of loops per timer tick. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/* Comparator for sleep_list: earlier wakeup_time first. */
static bool
wakeup_cmp (const struct list_elem *a,
            const struct list_elem *b,
            void *aux UNUSED) 
{
  const struct thread *t1 = list_entry(a, struct thread, elem);
  const struct thread *t2 = list_entry(b, struct thread, elem);
  return t1->wakeup_time < t2->wakeup_time;
}

void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
  list_init(&sleep_list);
}

void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;
  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;
  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

int64_t
timer_ticks (void) 
{
  enum intr_level old = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old);
  return t;
}

int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

void
timer_sleep (int64_t sleep_ticks) 
{
  if (sleep_ticks <= 0)
    return;

  int64_t start = timer_ticks ();
  struct thread *cur = thread_current ();
  cur->wakeup_time = start + sleep_ticks;

  enum intr_level old = intr_disable ();
  /* Insert into sleep_list in wakeup order and block. */
  list_insert_ordered(&sleep_list, &cur->elem, wakeup_cmp, NULL);
  thread_block ();
  intr_set_level (old);
}

void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  /* Unblock any threads whose wakeup_time has arrived. */
  while (!list_empty (&sleep_list))
    {
      struct thread *t = list_entry (list_front (&sleep_list),
                                     struct thread, elem);
      if (t->wakeup_time > ticks)
        break;
      list_pop_front (&sleep_list);
      thread_unblock (t);
    }
}

static bool
too_many_loops (unsigned loops) 
{
  int64_t start = ticks;
  while (ticks == start)
    barrier ();
  start = ticks;
  busy_wait (loops);
  barrier ();
  return start != ticks;
}

static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

static void
real_time_sleep (int64_t num, int32_t denom) 
{
  int64_t t = num * TIMER_FREQ / denom;
  ASSERT (intr_get_level () == INTR_ON);
  if (t > 0)
    timer_sleep (t);
  else
    real_time_delay (num, denom);
}

static void
real_time_delay (int64_t num, int32_t denom) 
{
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
}

