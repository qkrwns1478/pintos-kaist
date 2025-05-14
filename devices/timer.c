#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <stdint.h>

int64_t next_tick_to_awake = INT64_MAX; // 슬립 리스트 중 최소 일어날 tick을 저장

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19 // TIMER_FREQ는 19보다 커야 함
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* thread.c에서 정의된 sleep_list: 잠자는 스레드 목록 */
extern struct list sleep_list;

/* 슬립 리스트에서 깨어날 스레드를 깨우는 함수 */
void thread_awake(int64_t ticks);


/* Number of timer ticks since OS booted. */
static int64_t ticks; 

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS가 부팅된 이후로 지난 timer tick(일정한 주기로 발생시키는 인터럽트 1회) 수를 반환하는 함수 */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable (); // (1) 인터럽트를 비활성화하고 이전 상태 저장
	int64_t t = ticks;                          // (2) 전역 변수 ticks 값을 안전하게 읽어옴
	intr_set_level (old_level);                // (3) 이전 인터럽트 상태로 복원
	barrier ();                                // (4) 컴파일러의 명령어 재배치를 막기 위한 메모리 장벽
	return t;                                  // (5) 읽어온 tick 값 반환
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
  if (ticks <= 0) return;

  int64_t start = timer_ticks ();
  enum intr_level old_level = intr_disable ();   // 인터럽트 비활성화
  thread_sleep (ticks + start);                  // (1) 깨어날 tick 설정 및 슬립 리스트에 추가
  intr_set_level (old_level);                    // 인터럽트 상태 복구
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void 
timer_interrupt(struct intr_frame *args UNUSED) {
	ticks++; 

	if (loops_per_tick > 0)
	  thread_awake(ticks);
  
	thread_tick();
  }
  

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
   /* 주어진 loop 횟수로 busy_wait()을 돌렸을 때
   한 틱 이상 시간이 경과하는지를 판단하는 함수.
   즉, loop 수가 너무 많은지를 확인함. */

   static bool too_many_loops (unsigned loops) { 
	int64_t start = ticks; // (1) 현재 시점을 tick 단위로 저장
	while (ticks == start) // (2) tick 값이 변하지 않을 동안 대기
		barrier(); // 컴파일러 최적화를 막기 위한 메모리 장벽


	start = ticks; // (3) tick이 하나 증가한 시점부터 다시 시작
	busy_wait(loops);// (4) 주어진 loop만큼 바쁜 대기 (시간 측정용)

	barrier(); // (5) busy_wait 이후의 컴파일러 최적화 방지
	return start != ticks;  // (6) busy_wait 중 tick이 또 변했는지 확인
	// → true면 loops가 너무 많아 1 tick 이상 소요됨
}


/* Iterates through a simple loop LOOPS times, for implementing
   brief delays. */
static void NO_INLINE busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000); 
	}
}