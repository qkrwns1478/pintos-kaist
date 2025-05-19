#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif
/*
/*
Pintos 프로젝트 2는 사용자 프로그램이 커널에게 요청(system call)할 수 있도록
시스템 호출 인터페이스를 구현하는 것이 핵심이다.

이때 핵심 연결고리가 되는 것이 바로 **인터럽트 핸들러**이다.
사용자 프로그램이 시스템 콜을 호출하면, 소프트웨어 인터럽트 `0x30`이 발생하며
CPU는 커널 모드로 진입하고, 커널은 `userprog/syscall.c`에서 등록된
인터럽트 핸들러 `syscall_handler()`를 실행한다.

이 인터럽트 핸들러는 커널이 **사용자 프로그램의 CPU 상태(레지스터 값 포함)**를
저장한 구조체 `struct intr_frame *f`를 인자로 받는다. 이 구조체에는
시스템 콜 번호와 인자들이 저장된 **사용자 스택의 포인터(rsp)**도 들어 있다.

→ 커널은 이 `intr_frame`을 통해 **사용자 프로그램의 스택에 접근**하여
   시스템 콜 번호와 인자들을 읽어오고, 파일 열기, 읽기, 쓰기 등
   파일 시스템 관련 동작을 수행한 뒤 결과를 다시 레지스터에 저장해 사용자 모드로 복귀한다.

---
[💡 시스템 콜 처리 과정에서 argv[]는 무엇을 받는가?]

`argv[]`는 사용자가 실행한 명령어의 **각 토큰을 나눈 문자열 배열**이다.
예를 들어 사용자가 다음과 같이 명령어를 입력했다고 가정하자:

    run echo hello world

그러면 `file_name = "echo hello world"`가 되고, 이 문자열은 `load()` 함수 안에서
`strtok_r()`로 공백 기준으로 분할된다.

→ 다음과 같은 형태로 저장된다:

    argv[0] = "echo"       // 실행할 프로그램 이름
    argv[1] = "hello"      // 첫 번째 인자
    argv[2] = "world"      // 두 번째 인자
    ...
    argv[argc] = NULL;     // 마지막은 NULL로 끝남

그리고 이 `argv[]`는 사용자 스택에 문자열 복사 → 포인터 배열 복사 순으로
차례대로 푸시되며, 최종적으로 다음을 만족하게 된다:

    - 스택에는 문자열 데이터들("echo", "hello", "world")이 존재하고
    - 그 뒤에 해당 문자열 주소들이 포인터 배열로 쌓이며
    - 마지막에 argc와 fake return address(0)이 삽입된다

→ 이 과정을 통해 `main(int argc, char **argv)`에 정확히 전달될 수 있게 스택이 준비된다.

---
즉, 인터럽트 큐(= 인터럽트 발생 시 CPU 상태 저장 영역)는
사용자 요청 → 커널 진입 → 사용자 상태 저장 → 시스템 호출 처리 → 복귀
이 흐름의 중간 지점으로, 시스템 콜 처리를 가능하게 하는 기반 역할을 한다.
*/

static void process_cleanup (void); // 자원 회수. | 메모리 누수 방지
static bool load (const char *file_name, struct intr_frame *if_); // 파일 이름과 인터럽트 프레임을 인자로 받는다. | 인터럽트 프레임 : 인터럽트 발생 시, CPU의 레지스터 상태를 저장해두는 구조체
static void initd (void *f_name); // initd를 실행하는 함수. 유저프로세스를 띄우는 최초의 진입점
static void __do_fork (void *); // fork()를 수행하는 함수 | 현재 프로세스(부모)의 실행 컨텍스트와 메모리 정보를 복제하여 자식 프로세스를 만드는 함수.


struct lock process_lock; // 프로세스 락 | 경쟁 조건(Race Condition) 을 막지 않으면, 파일 시스템이 손상되거나 예측 불가능한 결과가 발생할 수 있음

/* General process initializer for initd and other process. */
static void
process_init (void) {  // 프로세스 초기화 함수
	struct thread *current = thread_current (); // 현재 스레드(프로세스)의 정보를 가져온다.
	lock_init(&process_lock); // 프로세스 락 초기화
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) { // initd를 실행하는 함수
	char *fn_copy; // initd를 실행하기 위한 파일 이름을 복사할 버퍼
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0); 
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	return thread_create (name, PRI_DEFAULT, __do_fork, thread_current ());
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	//void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */

	/* 2. Resolve VA from the parent's page map level 4. */
	//parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) { 
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	process_init ();

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();
		struct thread *curr = thread_current ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	
	/* TODO: Your code goes here. */
	lock_acquire(&process_lock);
    list_remove(&(curr->child_elem));
    lock_release(&process_lock);
	
	/* Print termination message. */
    if (curr->run_file != NULL)
        printf("%s: exit(%d)\n", curr->name, curr->exit_status);	/* TODO: project2/process_termination.html). */
	
	/* TODO: We recommend you to implement process resource cleanup here. */
	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Make a copy of file_name for parsing */
	char *fn_copy = palloc_get_page (0); // 임시 버퍼 할당
	char *save_ptr; // strtok_r을 위한 저장 포인터 | 토큰화된 문자열을 저장

	if (fn_copy == NULL)
		goto done;
	strlcpy(fn_copy, file_name, PGSIZE);
	char *prog_name = strtok_r(fn_copy, " ", &save_ptr);

	/* Open executable file. */
	file = filesys_open (prog_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", prog_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", prog_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	
	/* Argument parsing and pushing to the stack */
	// 현재 스레드의 이름(thread->name)을 복사해 토큰화 준비
	char *token;
	char *argv[128];
	int argc = 0;

	if (fn_copy == NULL)
		return false; // 실패 시 false 반환

	/* Tokenize arguments */
	strlcpy(fn_copy, file_name, PGSIZE);
	token = strtok_r(fn_copy, " ", &save_ptr);
	while (token != NULL) {
		argv[argc++] = token;
		token = strtok_r(NULL, " ", &save_ptr);
	}

	uintptr_t rsp = if_->rsp;

	/* Push arguments to stack (in reverse order) */
	char *arg_ptrs[128];
	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen(argv[i]) + 1;
		if_->rsp -= len;
		memcpy((void *)if_->rsp, argv[i], len);
		arg_ptrs[i] = (char *)if_->rsp;
	}

	/* Word-align */
	while (if_-> rsp % 8 != 0)
		if_-> rsp--;

	/* Push null sentinel */
	if_-> rsp -= sizeof(char *);
	*(char **)if_-> rsp = NULL;

	/* Push argument pointers */
	for (int i = argc - 1; i >= 0; i--) {
		if_->rsp -= sizeof(char *);
		*(char **)if_->rsp = arg_ptrs[i];
	}

	/* Push argv (char **) */
	char **argv_addr = (char **)if_->rsp;
	if_->rsp -= sizeof(char **);
	*(char ***)if_->rsp = argv_addr;

	/* Push argc */
	if_->rsp -= sizeof(int);
	*(int *)if_->rsp = argc;

	/* Push fake return address */
	if_->rsp -= sizeof(void *);
	*(void **)if_->rsp = NULL;

	success = true;

done:
	/*
	실행 중인 file을 닫음
	deny write on executables(Pintos에서 실행 중인 사용자 프로그램이 실행 파일 자체를 수정하지 못하도록 쓰기(write)를 금지하는 보안 메커니즘 | 프로세스 중복실행 방지) 
	를 위해 실행 중인 파일을 계속 open해 놓는다
	process 종료하기 전에 닫음

	*/

	/* We arrive here whether the load is successful or not. */
	file_close (file);
	palloc_free_page (fn_copy);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage, // 사용자 가상 주소
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) { // 읽을 바이트 수, 0으로 초기화할 바이트 수
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0); // 페이지 크기 단위로 나누어 떨어져야 함
	ASSERT (pg_ofs (upage) == 0); // 페이지 오프셋이 0이어야 함
	ASSERT (ofs % PGSIZE == 0); // 파일 오프셋이 페이지 크기 단위로 나누어 떨어져야 함

	file_seek (file, ofs); // 파일 포인터를 ofs 위치로 이동
	while (read_bytes > 0 || zero_bytes > 0) { // 읽을 바이트 수 또는 0으로 초기화할 바이트 수가 남아있을 때까지 반복
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE; // 읽을 바이트 수
		size_t page_zero_bytes = PGSIZE - page_read_bytes; // 0으로 초기화할 바이트 수

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER); // 사용자 공간에 페이지 할당
		if (kpage == NULL) /// 페이지 할당 실패 시
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;               // 사용자 스택에 할당할 커널 페이지의 포인터
	bool success = false;         // 페이지 매핑 성공 여부를 저장할 변수

	// PAL_USER: 사용자 공간에 할당, PAL_ZERO: 페이지를 0으로 초기화
	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		// USER_STACK - PGSIZE 위치(스택 최상단 1페이지)에 페이지를 매핑하고 쓰기 가능하게 설정
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);

		if (success)
			// rsp(스택포인터 : 스택 메모리에서 값을 가져오거나(push/pop), 값을 넣을 때 위치를 지정하는 데 사용 됨) 
			// 를 사용자 스택 최상단 주소(0x47480000 등)로 초기화
			if_->rsp = USER_STACK;
		else
			// 페이지 매핑 실패 시 메모리 누수 방지를 위해 반환
			palloc_free_page (kpage);	
	}

	// 페이지 매핑 또는 할당 실패 시 false 반환
	if (!success)
		return false;

	// 스택 설정 성공 시 true 반환
	return true;
}


/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool 
install_page (void *upage, void *kpage, bool writable) { 
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) { // 페이지를 로드하는 함수
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */