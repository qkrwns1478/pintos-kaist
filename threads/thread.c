#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
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
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

// 고유 스레드 ID 할당
/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */


static struct list sleep_list; // 슬립 상태 스레드를 저장할 리스트
bool cmp_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux);


/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
// 스레드 구조체 초기화
static void init_thread (struct thread *, const char *name, int priority);
// 스케줄링 수행
static void do_schedule(int status);
static void schedule (void);
// 고유 스레드 ID 할당
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
// 현재 실행 중인 스레드 반환
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
// 스레드 생성
   thread_create().

// 현재 실행 중인 스레드 반환
   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
// 조건 확인 (ASSERT 실패 시 커널 패닉)
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
// 락 초기화
	lock_init (&tid_lock);
// 리스트 초기화
	list_init (&ready_list);
// 리스트 초기화
	list_init (&destruction_req);
// 리스트 초기화
	list_init (&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
// 스레드 구조체 초기화
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
// 고유 스레드 ID 할당
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
// 세마포어 초기화
	sema_init (&idle_started, 0);
// 스레드 생성
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
// 인터럽트 활성화
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
// 세마포어 대기
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
// 현재 실행 중인 스레드 반환
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
// 스레드 생성
   scheduled before thread_create() returns.  It could even exit
// 스레드 생성
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
// 스레드 생성
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (function != NULL);

	/* Allocate thread. */
// 페이지 할당 (스레드 구조체용)
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
// 스레드 구조체 초기화
	init_thread (t, name, priority);
// 고유 스레드 ID 할당
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
// BLOCKED 상태 스레드를 READY로 전환
	thread_unblock (t);

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
// BLOCKED 상태 스레드를 READY로 전환
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
// 현재 스레드를 BLOCKED 상태로 전환
thread_block (void) {
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (!intr_context ());
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (intr_get_level () == INTR_OFF);
// 현재 실행 중인 스레드 반환
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
// 현재 스레드 CPU 양보
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
// BLOCKED 상태 스레드를 READY로 전환
thread_unblock (struct thread *t) {
	enum intr_level old_level;

// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (is_thread (t));

// 인터럽트 비활성화
	old_level = intr_disable ();
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (t->status == THREAD_BLOCKED);
// 리스트 뒤에 요소 삽입
	list_insert_ordered (&ready_list, &t->elem);
	t->status = THREAD_READY;
	if (!intr_context() && thread_current()->priority < t->priority){
		thread_yield();
	}
// 인터럽트 상태 복원
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
// 현재 실행 중인 스레드 반환
	return thread_current ()->name;
}

/* Returns the running thread.
// 현재 실행 중인 스레드 반환
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
// 현재 실행 중인 스레드 반환
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (is_thread (t));
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
// 현재 실행 중인 스레드 반환
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
// 현재 스레드를 종료
thread_exit (void) {
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
// 인터럽트 비활성화
	intr_disable ();
// 스케줄링 수행
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
// 현재 스레드 CPU 양보
thread_yield (void) {
// 현재 실행 중인 스레드 반환
	struct thread *curr = thread_current ();
	enum intr_level old_level;

// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (!intr_context ());

// 인터럽트 비활성화
	old_level = intr_disable ();
	if (curr != idle_thread)
// 리스트 뒤에 요소 삽입
		list_push_back (&ready_list, &curr->elem);
// 스케줄링 수행
	do_schedule (THREAD_READY);
// 인터럽트 상태 복원
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
// 현재 실행 중인 스레드 반환
	thread_current ()->priority = new_priority;
	if(!list_empty(&ready_list)){
 		struct thread *front = list_entry(list_front(&ready_list), struct thread, elem);
		if (thread_current()->priority < front->priority)
			thread_yield();
	}
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
// 현재 실행 중인 스레드 반환
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

// 현재 실행 중인 스레드 반환
	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
// 인터럽트 비활성화
		intr_disable ();
// 현재 스레드를 BLOCKED 상태로 전환
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
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (function != NULL);

// 인터럽트 활성화
	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
// 현재 스레드를 종료
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
// 스레드 구조체 초기화
init_thread (struct thread *t, const char *name, int priority) {
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (t != NULL);
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
// 조건 확인 (ASSERT 실패 시 커널 패닉)
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
// 조건 확인 (ASSERT 실패 시 커널 패닉)
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
// 스케줄링 수행
do_schedule(int status) {
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (intr_get_level () == INTR_OFF);
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
// 현재 실행 중인 스레드 반환
	thread_current ()->status = status;
	schedule ();
}

static void schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (intr_get_level () == INTR_OFF);
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (curr->status != THREAD_RUNNING);
// 조건 확인 (ASSERT 실패 시 커널 패닉)
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
// 현재 스레드를 종료
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
// 조건 확인 (ASSERT 실패 시 커널 패닉)
			ASSERT (curr != next);
// 리스트 뒤에 요소 삽입
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
// 고유 스레드 ID 할당
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}


bool cmp_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *t_a = list_entry(a, struct thread, elem);
	struct thread *t_b = list_entry(b, struct thread, elem);
	return t_a->wakeup_tick < t_b->wakeup_tick;
	}



void thread_sleep (int64_t wakeup) {
	enum intr_level old_level = intr_disable();  // 🔐 인터럽트 OFF
	struct thread *curr =  thread_current();
	curr->wakeup_tick = wakeup; // 현재 thread 구조체 wakeup_tick 필드에 wakeup 값 입력
	list_insert_ordered(&sleep_list, &curr->elem, cmp_wakeup_tick, NULL); //
	thread_block();
	intr_set_level(old_level);  // 🔓 원래 인터럽트 상태 복원
}

void thread_awake(int64_t now_tick) {
	while(!list_empty(&sleep_list)) {
		struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);

		// 깨어날 시간이 된 경우
		if (t->wakeup_tick <= now_tick) {
			list_pop_front(&sleep_list); // 리스트에서 제거
			thread_unblock(t);  // READY 상태로 변경
		} else {
			break; // 리스트는 정렬되어 있으므로 더 이상 검사할 필요 X
		}
	}
}

bool cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry(a, struct thread, elem)->priority > list_entry(b, struct thread, elem)->priority;
}

void donation_priority(struct thread *t); {
	if (t->wait_on_lock == NULL) return; // 1단계: wait_on_lock이 없다면 아무것도 하지 않음.

	struct lock *lock = t->wait_on_lock;
	struct thread *holder = lock->holder;

	if (holder != NULL && holder->priority < t->priority) { // 2단계: holder가 있고, holder의 우선순위가 t보다 낮다면 donation
		holder->priority = t->priority;
	
		donation_priority(holder); // 재귀 호출로 nested donation 처리
	}
}

void remove_with_lock(struct lock *lock) {
	struct list_elem *e = list_begin(&thread_current()->donations);

	while(e != list_end(&thread_current()->donations)) {
		struct thread *t = list_entry(e, struct thread, donation_elem);
		struct list_elem *next = list_next(e);

		if (t->wait_on_lock == lock)
			list_remove(e);
		e = next;
	}
}


void refresh_priority(void) {
	struct thread *curr = thread_current(); // 현재 스레드의 priority를 init_priority로 복원
	curr->priority = curr->init_priority;

	if (!list_empty(&curr->donations)) {   // donation 리스트 중 가장 높은 priority를 찾아서
		list_sort(&curr->donations, cmp_priority, NULL);  // 현재 priority에 반영(init_priority 보다 높은 경우만)
		struct thread *donor = list_entry(list_front(&curr->donations), struct thread, donation_elem);
		if (donor->priority > curr->priority)
			curr->priority = donor->priority;
	}
}