#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드 */
	THREAD_READY,       /* 실행 중은 아니지만, 실행 준비가 된 스레드 */
	THREAD_BLOCKED,     /* 특정 이벤트가 발생하길 기다리는 중인 스레드 */
	THREAD_DYING        /* 곧 종료되어 제거될 예정인 스레드 */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0         // 가장 낮은 우선순위
#define PRI_DEFAULT 31    // 기본 우선순위 (보통 새 스레드는 이 값으로 시작)
#define PRI_MAX 63        // 가장 높은 우선순위

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */


 /*

 이 4KB 페이지의 맨 아래(0KB) 에 struct thread 구조체가 위치합니다.
 나머지 공간은 커널 스택(kernel stack) 용도로 사용되며, 위에서 아래로(grows downward) 확장됨

*/

 /*magic:
 스레드가 유효한지 확인하는 데 쓰이는 값. 스택 오버플로우가 발생하면 이 값이 덮여져서 문제를 감지할 수 있습니다.
 
 intr_frame:
 인터럽트 발생 시의 CPU 상태 저장 구조체. 문맥 전환 시 중요.
 
 kernel stack:
 스레드가 커널 모드에서 사용하는 스택. 오버플로우 시 magic 필드를 침범하므로 주의.

*/

/*
1. 위에서 자라는 스택이 아래에 있는 thread를 덮으면 magic 값이 망가지므로 탐지 가능
2. 4KB = 페이지 단위, 스레드당 1개 페이지 쓰면 메모리 보호 및 관리 쉬움
3. 스레드 정보와 스택을 한 공간에 묶어두면 할당/해제 관리가 쉬움
*/

struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */
	struct list_elem elem; // ready_list, sleep_list에 엘레멘트를 언제 담아야 할지 판별
	int64_t wakeup_tick;  // 스레드가 언제 일어나야할지를 저장하는 필드 추가함



#ifdef USERPROG //  USERPROG 메크로가 정의되어 있을 경우에만 사용될 것 | pml4(Page Map Level 4, 사용자 주소 공간)가 사용되므로,
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4  | x86-64 아키텍처의 4단계 페이지 테이블 중 가장 상위 단계 */
/*
PML4 : 최상위 테이블. PDPT를 가리킴
PDPT : 페이지 디렉터리 포인터 테이블. PML4를 가리킴
PD : 페이지 디렉터리. PT(4단계)를 가리킴
PT : 페이지 테이블. 실제 물리 메모리 주소를 가리킴
*/

#endif
#ifdef VM // VM 메크로가 정의되어 있을 경우에만 사용될 것 | 가상 메모리 사용
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt; // 가상 메모리 페이지 테이블
#endif // 메크로 사용 종료

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs; // 다중 레벨 피드백 큐 스케줄러 사용 여부
extern int64_t next_tick_to_awake; // 다음에 깨워야 할 스레드의 tick

void thread_init (void); // 스레드 초기화
void thread_start (void); // 스레드 시작

int64_t reset_tick(int64_t tick); // tick을 초기화 하고 반환
void thread_sleep(int64_t ticks); // ticks 만큼 잠자기
void thread_awake(int64_t ticks); // ticks 만큼 잠자고 있는 스레드 깨우기


void thread_tick (void); // 타이머 인터럽트 발생 시 호출되는 함수로, 통계 갱신 및 time slice 체크
void thread_print_stats (void); // 현재까지 누적된 스레드 관련 통계를 출력
typedef void thread_func (void *aux); // 커널 스레드에서 실행할 함수의 타입 정의

tid_t thread_create (const char *name, int priority, thread_func *, void *); 
// 새로운 커널 스레드를 생성하고 ready_list에 추가, thread_func 실행

void thread_block (void); // 현재 스레드를 BLOCK 상태로 전환하고 스케줄링
void thread_unblock (struct thread *); // BLOCK 상태의 스레드를 READY 상태로 전환하여 ready_list에 삽입

struct thread *thread_current (void); // 현재 CPU에서 실행 중인 스레드 구조체를 반환
tid_t thread_tid (void); // 현재 실행 중인 스레드의 tid 반환
const char *thread_name (void); // 현재 실행 중인 스레드의 이름 반환
void thread_exit (void) NO_RETURN; // 현재 스레드를 종료하고 다른 스레드로 전환, 절대 복귀하지 않음
void thread_yield (void); // 현재 스레드를 READY 상태로 만들고 CPU를 양보 (스케줄러 호출)

int thread_get_priority (void); // 현재 스레드의 우선순위(priority)를 반환
void thread_set_priority (int); // 현재 스레드의 우선순위를 지정한 값으로 설정

bool priority_greater(const struct list_elem *, const struct list_elem *, void *); 
// 우선순위를 비교하는 함수로, list_insert_ordered에서 사용됨

int thread_get_nice (void); // 현재 스레드의 nice 값 반환 (MLFQS용)
void thread_set_nice (int); // 현재 스레드의 nice 값을 설정 (MLFQS용)

int thread_get_recent_cpu (void); // 현재 스레드의 recent_cpu 값 반환 (MLFQS용)
int thread_get_load_avg (void); // 시스템의 load_avg 값을 반환 (MLFQS용)

void do_iret (struct intr_frame *tf); // 스레드의 레지스터 상태를 복구하여 사용자 프로그램으로 복귀

bool priority_cmp(const struct list_elem *a, const struct list_elem *b, void *aux);
#endif /* threads/thread.h */
