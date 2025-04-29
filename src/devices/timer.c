#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <list.h>
#include "threads/fixed-point.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

struct sleepingThread{
  struct thread *t; //The thread that is sleeping
  int64_t wakeup_tick; //holds the time thread should be woken up
  struct semaphore sema; //semaphore value initialized to 0 to block thread
  struct list_elem elem; //node so that thread can be added to a linked list
};


/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

static struct list sleeping_threads; // global list to store sleeping threads
/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
  list_init(&sleeping_threads); //Initalize the sleeping threads list
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (loops_per_tick | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

//compare function to so that it can compare the wake up time so that it can be inserted into a sorted list
static _Bool cmp_function(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  //pointers to the threads we want to compare
  struct sleepingThread *thread_a = list_entry(a, struct sleepingThread, elem);
  struct sleepingThread *thread_b = list_entry(b, struct sleepingThread, elem);
  return thread_a->wakeup_tick < thread_b->wakeup_tick; //returns where the wakeup time for thread a is less than thread b
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();
  int64_t wakeup_time = start + ticks; //calcuate wake up time by adding its start time 
                                      //with the amount of ticks it should sleep for
  struct sleepingThread thread_entry; //create a sleeping tread struct to add to sleeping thread list
  //add current thread, wake up time to sleeping thread struct
  thread_entry.t = thread_current(); 
  thread_entry.wakeup_tick = wakeup_time; 
  sema_init(&thread_entry.sema, 0); // initialize sema value to 0

  //current_level used to indicate if interupts are disable or enable and disable interrupts using intr_disable()
  enum intr_level current_level = intr_disable();

  //inserts thread into sleeping list in a sorted order so that the timer interupt only needs to check first element in a list
  list_insert_ordered(&sleeping_threads, &thread_entry.elem, cmp_function, NULL);
  
  //use intr_set_level function to turn interupts back on
  intr_set_level(current_level);
  //used sema_down to block thread until timer interrupt preforms sema_up to wake thread up
  sema_down(&thread_entry.sema);
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  /* MLFQS calculations */
  if (thread_mlfqs)
  {
    /* Increment recent_cpu every tick */
    mlfqs_increment_recent_cpu();
    
    /* Update priorities every 4 ticks */
    if (ticks % 4 == 0)
    {
      mlfqs_recalculate_priorities();
    }
    
    /* Update load_avg and recent_cpu every second */
    if (ticks % TIMER_FREQ == 0)
    {
      mlfqs_calculate_load_avg();
      mlfqs_recalculate_recent_cpu();
      mlfqs_recalculate_priorities(); /* Recalculate priorities again after recent_cpu update */
    }
    
    /* CRITICAL: Use intr_yield_on_return instead of thread_yield in interrupt context */
    intr_yield_on_return();
  }

  while(!list_empty(&sleeping_threads)){
    //gets the first thread element in the sleeping threads list
    struct list_elem *element = list_front(&sleeping_threads);

    //create pointer to sleepingThread using list element
    struct sleepingThread *firstThread = list_entry(element, struct sleepingThread, elem);
    //since the list is ordered check the wake up time for the first thread in sleeping list
    //if its greater than the ticks it mean no threads need to be woken up and break the loop
    if(firstThread->wakeup_tick > ticks) break; 

    //if wakeup time is greater than or equal to ticks it should be woken up
    //pop the first thread on list
    list_pop_front(&sleeping_threads);

    //wake up the thread by signaling its semaphore up
    sema_up(&firstThread->sema);
  }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}