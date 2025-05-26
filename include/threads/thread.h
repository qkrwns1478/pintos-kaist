#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif
#define USERPROG true // for debugging

/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#define NICE_DEFAULT 0
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0

#define FILED_MAX 128					/* Maximum # of file descriptors. */

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
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

	/* New field for local tick */
	int64_t wakeup_tick;				/* tick till wake up */

	/* New field for initial priority */
	int priority_ori;

	/* Data structure for Multiple Donation */
	struct list donations;				/* List of Donors */
	struct list_elem d_elem;			/* List element for donations */

	/* Data structure for Nested Donation */
	struct lock *wait_on_lock;			/* lock that it waits for */

	/* Values for the advanced scheduler */
	int nice;
	int recent_cpu;

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */

	/* File Descriptor Table */
	struct file *fdt[FILED_MAX];				/* List of pointer to struct file */
	// int next_fd;
	
	int exit_status;
	struct thread *parent;				/* Parent of this thread */
	struct list children;				/* List of children this thread has */
	struct child *child_info;			/* Information of this thread as someone's child */

	struct file *running_file;
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

#ifdef USERPROG
struct child {
	tid_t tid;
	int exit_status;
	bool is_waited;
	bool is_exit;
	bool fork_fail;
	struct list_elem c_elem;
	struct semaphore c_sema;
};
#endif

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
int thread_get_priority_ori (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

void thread_sleep (int64_t ticks);
void thread_wakeup (int64_t ticks);
bool cmp_tick (struct list_elem *a, struct list_elem *b, void *aux UNUSED);
bool cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
bool cmp_priority_donate (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
int get_highest_priority (void);
void do_preemption (void);
void thread_refresh_priority (void);

int calc_priority(int recent_cpu, int nice);
int calc_load_avg (void);
int calc_recent_cpu (struct thread *t);
int ready_threads (void);
int itof(int n);
int ftoi(int x);
int add_xy(int x, int y);
int sub_xy(int x, int y);
int add_xn(int x, int n);
int sub_xn(int x, int n);
int mul_xy(int x, int y);
int mul_xn(int x, int n);
int div_xy(int x, int y);
int div_xn(int x, int n);
int read_sign_bit(int x);
int write_sign_bit(int x, int s);

#ifdef USERPROG
struct child *init_child (tid_t tid);
struct child *get_child_by_tid (tid_t tid);
#endif

#endif /* threads/thread.h */
