#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/vaddr.h"
#include "threads/synch.h"

void syscall_entry (void); // 시스템 콜 엔트리 포인트 | 엔트리 포인트? : 시스템 콜을 처리하기 위한 진입점
void syscall_handler (struct intr_frame *); // 시스템 콜 핸들러 | 시스템 콜을 처리하는 함수
bool is_valid (const void *addr); // 유효한 주소인지 확인하는 함수

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

struct lock filesys_lock;

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	switch(f->R.rax) {
		case SYS_HALT:                   /* Halt the operating system. */
			halt();
			break;
		case SYS_EXIT:                   /* Terminate this process. */
			exit(f->R.rdi);
			break;
		case SYS_FORK:                   /* Clone current process. */
			f->R.rax = fork(f->R.rdi, f);
			break;
		case SYS_EXEC:                   /* Switch current process. */
			// f->R.rax = exec(f->R.rdi);
			break;
		case SYS_WAIT:                   /* Wait for a child process to die. */
			// f->R.rax = wait(f->R.rdi);
			break;
		case SYS_CREATE:                 /* Create a file. */
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:                 /* Delete a file. */
			f->R.rax = remove(f->R.rdi);
			break;
		case SYS_OPEN:                   /* Open a file. */
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:               /* Obtain a file's size. */
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:                   /* Read from a file. */
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:                  /* Write to a file. */
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:                   /* Change position in a file. */
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:                   /* Report current position in a file. */
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:                  /* Close a file. */
			close(f->R.rdi);
			break;
		default:
			exit(f->R.rdi);
	}
}

/* Shutdown pintos. */
void halt (void) {
	power_off();
}

/* Exit process. */
void exit(int status) {
	struct thread *curr = thread_current();
	curr->exit_status = status;
    printf("%s: exit(%d)\n", curr->name, status);
	thread_exit();
}

/* Create new process which is the clone of current process with the name THREAD_NAME. */
pid_t fork (const char *thread_name, struct intr_frame *if_) {
	/* THREAD_NAME이라는 이름으로 현재 프로세스의 복사본을 생성한다.
	피호출자가 저장한 레지스터인 %RBX, %RSP, %RBP, %R12 - %R15는 반드시 그 값을 복사해야 하지만, 나머지는 그럴 필요는 없다.
	자식 프로세스의 pid를 리턴해야만 하며, 그렇지 않은 경우 유효한 pid를 가지면 안 된다.
	자식 프로세스에서는 리턴값이 0이어야 한다.
	자식 프로세스는 파일 디스크립터와 VM 공간 등을 포함해 복제된 리소스를 가져야 한다.
	부모 프로세스는 자식 프로세스가 성공적으로 복제된 것을 알기 전까지는 fork로부터 리턴하면 안 된다. 
	즉, 자식 프로세스가 자원을 복제하는 데 실패했다면, 부모의 fork() call은 TID_ERROR를 리턴해야 한다.
	템플릿은 대응하는 페이지 테이블 구조를 포함한 전체 유저 메모리 공간을 복사하는데 pml4_for_each()를 사용하지만,
	pte_for_each_func의 빠진 부분을 채워야 한다. */

	if (!is_valid(thread_name)) exit(-1);
	return process_fork(thread_name, if_);
}

/* Create child process and execute program corresponds to cmd_file on it. */
int exec (const char *cmd_line) {
	/* 현재 프로세스를 cmd_line에 (어떤 인수와 함께) 주어진 이름의 실행 프로그램으로 바꾼다.
	성공하면 리턴하지 않지만, 어떤 이유로 프로그램을 로드하거나 실행할 수 없다면, 그 프로그램은 -1을 리턴하면서 종료된다.
	이 함수는 exec를 호출한 스레드의 이름을 바꾸지 않는다. 
	(주의) 파일 디스크립터는 exec가 호출된 이후에도 열려있다. */

	if (!is_valid(cmd_line)) exit(-1);

	return process_exec(cmd_line);
}

/* Wait for termination of child process whose process id is pid. */
int wait (pid_t pid) {
	/* 자식 프로세스 pid를 기다리고, 자식의 종료 상태를 회수한다. pid가 아직 살아 있다면, 종료될 때까지 기다린다.
	pid가 exit하면서 넘긴 상태값을 리턴한다.
	pid가 exit()을 호출하지 않았지만 (exception 등에 의해) 커널에 의해 종료되었다면, wait(pid)는 -1을 리턴한다.
	부모 프로세스가 wait을 호출했을 때 이미 종료된 자식 프로세스를 기다릴 수도 있다. 
	하지만 커널은 여전히 부모가 자식의 종료 상태를 회수하거나 자식이 커널에 의해 종료되었음을 알도록 해야 한다.
	다음과 같은 조건들이 참인 경우 wait은 즉시 실패하고 -1을 리턴해야 한다:
		- pid가 호출한 프로세스의 direct child를 참조하지 않는 경우:
			호출한 프로세스가 fork를 성공적으로 호출했을 때 pid를 리턴값으로 받을 때, pid는 호출한 프로세스의 direct child이다.
			A가 자식 B를 만들고, B가 자식 C를 만들면, A는 B가 죽어도 C를 wait할 수 없다는 점에서 자식은 상속되지 않는다.
			A에 의한 wait(C)는 실패해야 한다.
			비슷하게, 고아 프로세스들은 그들의 부모 프로세스가 (새로운 부모를 할당하기 전에) 종료한다면 새로운 부모를 가질 수 없다.
		- wait을 호출하는 프로세스가 이미 pid에 wait을 호출한 경우:
			즉, 프로세스는 어떤 주어진 자식에 대해 최대 한 번만 wait할 수 있다.
	프로세스들은 아무 수의 자식을 만들고, 아무 순서로 wait하고, 심지어 그 자식들 중 일부 또는 전체를 기다리지 않고 종료될 수 있다. 
	구현할 때 가능한 모든 경우를 고려해야 한다.
	부모가 기다리든지 말든지, 자식이 부모 전/후로 종료되던지 간에, struct thread를 포함한 프로세스의 모든 자원들은 free되어야 한다.
	최초의 프로세가 exit할 때까지는 핀토스가 종료되서는 안 된다. 제공된 핀토스는 main()에서 process_wait()를 호출해서 이 조건을 지키려고 한다. 
	process_wait()를 먼저 구현하고, process_wait()에 따라 wait 시스템 콜을 구현해 보자. */

	return process_wait(pid); // 
}

/* Create file which have size of initial_size. */
bool create (const char *file, unsigned initial_size) {
	// if (!is_user_vaddr(file)) exit(-1);
	// if (!pml4_get_page(thread_current()->pml4, file)) exit(-1);
	// if (file == NULL) exit(-1);
	if (!is_valid(file)) exit(-1);
	lock_acquire(&filesys_lock);
	bool res = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	return res;
}

/* Remove file whose name is file. */
bool remove (const char *file) {
	if (!is_valid(file)) exit(-1);
	lock_acquire(&filesys_lock);
	bool res = filesys_remove(file);
	lock_release(&filesys_lock);
	return res;
}

/* Open the file corresponds to path in "file". */
int open (const char *filename) {
	if (!is_valid(filename)) exit(-1);
	struct thread *curr = thread_current();
	int fd;
	bool is_not_full = false;
	for(fd = 2; fd < 64; fd++) {
		if (curr->fdt[fd] == NULL) {
			is_not_full = true;
			break;
		}
	}
	if (!is_not_full) return -1; // Return -1 if fdt is full

	lock_acquire(&filesys_lock);
	struct file *file = filesys_open(filename);
	lock_release(&filesys_lock);
	if (file == NULL) return -1; // Return -1 if file is not opened

	curr->fdt[fd] = file;
	return fd;
}

/* Return the size, in bytes, of the file open as fd. */
int filesize (int fd) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct file *file = thread_current()->fdt[fd];
	if (file == NULL) exit(-1);

	lock_acquire(&filesys_lock);
	off_t res = file_length(file);
	lock_release(&filesys_lock);
	return res;
}

/* Read size bytes from the file open as fd into buffer. */
int read (int fd, void *buffer, unsigned size) {
	if (size == 0) return 0;
	if (!is_valid(buffer)) exit(-1);
	if (fd == 0) { // fd0 is stdin
		char *buf = (char *) buffer;
		for (int i = 0; i < size; i++) {
			buf[i] = input_getc();
		}
		return size;
	}
	else if (fd == 1) exit(-1); // fd1 is stdout (invalid)
	else if (fd < 2 || fd > 63) exit(-1); // invalid fd
	else {
		struct file *file = thread_current()->fdt[fd];
		if (file == NULL) exit(-1);
		lock_acquire(&filesys_lock);
		off_t res = file_read(file, buffer, size);
		lock_release(&filesys_lock);
		return res;
	}
}

/* Writes size bytes from buffer to the open file fd. */
int write(int fd, const void *buffer, unsigned size) {
	if (!is_valid(buffer)) exit(-1);
	if(fd == 1) { // fd1 is stdout
		putbuf(buffer, size);
		return size;
	} else if (fd == 0) exit(-1); // fd0 is stdin (invalid)
	else if (fd < 2 || fd > 63) exit(-1); // invalid fd
	else {
		struct file *file = thread_current()->fdt[fd];
		if (file == NULL) exit(-1);
		lock_acquire(&filesys_lock);
		off_t res = file_write(file, buffer, size);
		lock_release(&filesys_lock);
		if (res < 0) return -1;
		return res;
	}
}

/* Changes the next byte to be read or written in open file fd to position. */
void seek (int fd, unsigned position) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1);
	lock_acquire(&filesys_lock);
	file_seek(file, position);
	lock_release(&filesys_lock);
}

/* Return the position of the next byte to be read or written in open file fd. */
unsigned tell (int fd) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1); // file not found
	lock_acquire(&filesys_lock);
	off_t res =  file_tell(file);
	lock_release(&filesys_lock);
	return res;
}

/* Close file descriptor fd. */
void close (int fd) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1);
	curr->fdt[fd] = NULL;
	// if (fd == curr->next_fd && curr->next_fd > 2) curr->next_fd--;
	lock_acquire(&filesys_lock);
	file_close(file);
	lock_release(&filesys_lock);
}

bool is_valid(const void *addr) {
	if (addr == NULL) return false;
	if (!is_user_vaddr(addr)) return false;
	if (pml4_get_page(thread_current()->pml4, addr) == NULL) return false;
	return true;
}