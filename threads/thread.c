#include "threads/thread.h"
#include <stdint.h>
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/thread.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list; // ready_list: 대기 중인 스레드 목록 ( 우선순위 정렬 )

/* 잠 재워질 스레드 리스트 */
static struct list sleep_list; // sleep_list: 잠자는 스레드 목록 (오름차순 정렬)


/* tick을 초기화 하고 반환  | 가장 빨리 찾아올 tick ( 스레드가 일어나야 할 시간 )*/
int64_t reset_tick(int64_t tick) { // tick을 초기화하고 반환하는 함수
	if (tick < next_tick_to_awake) // tick이 현재 저장된 tick보다 작다면
		next_tick_to_awake = tick; // tick을 next_tick_to_awake에 저장
	return next_tick_to_awake; // tick을 반환
}

/* tick 시간 비교 */
bool tick_wake_faster(const struct list_elem *a, const struct list_elem *b, void *aux); 
// tick_wake_faster 함수 선언: 슬립 리스트 정렬 시, 더 일찍 깨야 하는 스레드를 앞으로 배치하기 위한 비교 함수

/* priority 비교 */
bool priority_greater(const struct list_elem *a,       // ready_list 정렬 시 사용되는 첫 번째 요소 (list_elem 포인터)
                      const struct list_elem *b,       // ready_list 정렬 시 사용되는 두 번째 요소 (list_elem 포인터)
                      void *aux UNUSED) {              // 컴파일러 경고 무시

/* list_entry 매크로를 통해 list_elem 포인터를 실제 thread 포인터로 변환 */
const struct thread *t_a = list_entry(a, struct thread, elem); // 첫 번째 요소가 속한 thread 구조체 포인터 획득
const struct thread *t_b = list_entry(b, struct thread, elem); // 두 번째 요소가 속한 thread 구조체 포인터 획득

return t_a->priority > t_b->priority; // 우선순위가 높은 스레드가 앞으로 오도록 true/false 반환
}

/* 스레드 unlock */
void thread_unlock(struct thread *t); // 스레드의 lock을 해제하는 함수


/* Idle thread. */
static struct thread *idle_thread; // idle 스레드 | 대기 중인 스레드가 없을 때 실행되는 스레드

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread; // 초기 스레드 | 	PintOS에서 가장 처음 만들어지는 스레드를 가리키는 포인터

/* Lock used by allocate_tid(). */
static struct lock tid_lock; // TID 할당을 위한 lock | 스레드 생성 시 TID를 할당하기 위해 사용되는 lock

/* Thread destruction requests */
static struct list destruction_req; // 스레드 파괴 요청 리스트 | 스레드가 종료될 때, 자원을 해제하기 위한 요청을 저장하는 리스트

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. | idle 상태에서의 tick 수*/ 
static long long kernel_ticks;  /* # of timer ticks in kernel threads. | 커널 스레드 실행 시간 */
static long long user_ticks;    /* # of timer ticks in user programs. | 	사용자 프로그램 실행 시간 */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */ // 스레드에게 주는 타임 슬라이스(선점형 프로세스에서 연속으로 할당되는 시간의 단위)
static unsigned thread_ticks;   /* # of timer ticks since last yield. */ // 스레드가 마지막으로 양보한 이후의 타이머 틱 수


/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux); // 커널 스레드의 기본 함수

static void idle (void *aux UNUSED);  // idle 스레드의 기본 함수 | 스레드가 대기 중일 때 실행되는 함수
static struct thread *next_thread_to_run (void); // 다음에 실행할 스레드를 선택하는 함수
static void init_thread (struct thread *, const char *name, int priority); // 스레드 초기화 함수
static void do_schedule(int status); // 스레드 상태를 변경하는 함수
static void schedule (void); // 스레드 스케줄링 함수
static tid_t allocate_tid (void); // 스레드 ID를 할당하는 함수

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init(&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */

/* 새 커널 스레드를 생성하는 함수
   - name: 스레드 이름
   - priority: 스레드 우선순위
   - function: 새 스레드에서 실행할 함수
   - aux: 해당 함수에 전달할 인자 */
   tid_t
   thread_create (const char *name, int priority,
				  thread_func *function, void *aux) {
	   struct thread *t;       // 새로 생성할 스레드를 위한 포인터
	   tid_t tid;              // 새 스레드의 TID (Thread ID)
   
	   ASSERT (function != NULL); // 실행할 함수가 NULL이 아닌지 확인 (필수 인자)
   

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);  // 생성한 스레드를 ready_list에 넣고 READY 상태로 전환하여 스케줄링 가능하게 함

	return tid;          // 새로 생성한 스레드의 고유 ID를 반환

}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unlock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */

void
thread_block (void) {
	ASSERT (!intr_context ());               // 인터럽트 컨텍스트에서는 호출될 수 없음 (커널 코드 보호)
	ASSERT (intr_get_level () == INTR_OFF);  // 인터럽트가 비활성화된 상태여야 함 (동기화 보장)
	thread_current ()->status = THREAD_BLOCKED;  // 현재 스레드의 상태를 BLOCKED로 설정하여 실행 대상에서 제외
	schedule ();                             // 다음 실행할 스레드를 선택하고 문맥 전환 수행
}


#define NO_INLINE __attribute__ ((noinline))

// 현재 스레드를 지정된 tick까지 잠들게 하는 함수
void thread_sleep(int64_t wakeup_tick) {
	enum intr_level old_level = intr_disable();  // (1) 인터럽트 비활성화 (리스트 조작 중 예기치 않은 동시 접근 방지)
	struct thread *curr = thread_current();      // (2) 현재 실행 중인 스레드 가져오기

	if (curr != idle_thread) {  // (3) idle 스레드는 sleep할 필요 없음
		curr->wakeup_tick = wakeup_tick;  // (4) 깨어나야 할 tick 저장
		list_insert_ordered(&sleep_list, &curr->elem, tick_wake_faster, NULL);  // (5) sleep_list에 깨어날 시간 기준으로 정렬 삽입
		thread_block();  // (6) 현재 스레드를 BLOCK 상태로 전환하여 스케줄러로부터 제외
	}

	intr_set_level(old_level);  // (7) 인터럽트 상태 복구
}

// BLOCK 상태의 스레드를 READY 상태로 전환하여 ready_list에 삽입하는 함수
void thread_unblock(struct thread *t) {
	enum intr_level old_level;

	ASSERT(is_thread(t));                   // (1) 유효한 스레드인지 확인
	old_level = intr_disable();             // (2) ready_list 접근 전에 인터럽트 비활성화
	ASSERT(t->status == THREAD_BLOCKED);    // (3) 반드시 BLOCKED 상태여야 함

	list_insert_ordered(&ready_list, &t->elem, priority_greater, NULL);  // (4) 우선순위 기준으로 ready_list에 정렬 삽입
	t->status = THREAD_READY;               // (5) 스레드 상태를 READY로 설정

	intr_set_level(old_level);              // (6) 인터럽트 상태 복구
}

// 현재 tick 기준으로 깨어날 시간(wakeup_tick)이 지난 스레드들을 깨우는 함수
void thread_awake(int64_t current_tick) {
	while (!list_empty(&sleep_list)) {  // (1) sleep_list가 비어있지 않은 동안 반복
		struct list_elem *e = list_front(&sleep_list);              // (2) 리스트 앞의 요소 확인
		struct thread *t = list_entry(e, struct thread, elem);      // (3) list_elem을 thread 구조체로 변환

		if (t->wakeup_tick <= current_tick) {  // (4) 깨어나야 할 시간 도달한 경우
			list_pop_front(&sleep_list);       // (5) 리스트에서 제거
			thread_unblock(t);                 // (6) READY 상태로 전환
		} else {
			break;  // (7) sleep_list는 정렬되어 있으므로, 이후 스레드는 모두 나중에 깨어남 → 루프 종료
		}
	}

	// (8) 만약 더 높은 우선순위의 스레드가 ready_list에 있다면 현재 스레드는 양보 예약
	if (!list_empty(&ready_list)) {
		struct thread *highest = list_entry(list_front(&ready_list), struct thread, elem);  // (9) 가장 우선순위 높은 스레드 확인
		if (thread_current()->priority < highest->priority) {  // (10) 현재보다 높다면
			intr_yield_on_return();  // (11) 다음 시점에 CPU 양보
		}
	}
}

// sleep_list에 삽입 시 사용할 비교 함수
// 깨어날 시간이 더 빠른 스레드가 앞에 오도록 정렬 기준 제공
bool tick_wake_faster(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED) {
	const struct thread *a = list_entry(a_, struct thread, elem);  // (1) 첫 번째 요소를 thread로 변환 | 테스트 케이스에서 사용할 첫번째 스레드 
	const struct thread *b = list_entry(b_, struct thread, elem);  // (2) 두 번째 요소를 thread로 변환 | 테스트 케이스에서 사용할 두번째 스레드
	return a->wakeup_tick < b->wakeup_tick;  // (3) 깨어날 시간이 빠른 순으로 정렬 (오름차순)
}


/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unlock a thread and
   update other data. */
void
thread_unlock (struct thread *t) { // 스레드를 잠금을 해제하는 함수
	enum intr_level old_level; // 인터럽트 레벨을 저장하는 변수

	ASSERT (is_thread (t)); // t가 유효한 스레드인지 확인

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED); // t가 BLOCKED 상태인지 확인
	list_push_back (&sleep_list, &t->elem); // sleep_list에 t를 추가
	t->status = THREAD_READY; // t의 상태를 THREAD_READY로 변경
	intr_set_level (old_level); // 이전 인터럽트 레벨로 복원
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid; 
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {   // 현재 실행 중인 스레드가 자발적으로 CPU를 양보하고 싶을 때 호출 
						// 	ex) (1)  현재 스레드보다 우선순위가 높은 스레드가 ready_list에 있을 때
						//		(2)  timer_interrupt()에서 TIME_SLICE가 초과됐을 때 (타임 슬라이스 종료 → 선점)


	struct thread *curr = thread_current ();// 현재 실행 중인 스레드
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, priority_greater, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread (); // 현재 실행 중인 스레드
	struct thread *next = next_thread_to_run (); // 다음 실행할 스레드

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING; // 다음 스레드의 상태를 RuNNING으로 변경

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}