#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	switch(f->R.rax) {
		case SYS_HALT:                   /* Halt the operating system. */
			halt();
		case SYS_EXIT:                   /* Terminate this process. */
			exit(f->R.rdi);
		case SYS_FORK:                   /* Clone current process. */
			break;
		case SYS_EXEC:                   /* Switch current process. */
			break;
		case SYS_WAIT:                   /* Wait for a child process to die. */
			break;
		case SYS_CREATE:                 /* Create a file. */
			break;
		case SYS_REMOVE:                 /* Delete a file. */
			break;
		case SYS_OPEN:                   /* Open a file. */
			break;
		case SYS_FILESIZE:               /* Obtain a file's size. */
			break;
		case SYS_READ:                   /* Read from a file. */
			break;
		case SYS_WRITE:                  /* Write to a file. */
			printf("%s", f->R.rsi);
			// f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:                   /* Change position in a file. */
			break;
		case SYS_TELL:                   /* Report current position in a file. */
			break;
		case SYS_CLOSE:                  /* Close a file. */
			break;
		default:
			printf("Undefined system call(%d)\n", f->R.rax);
			exit(1);
	}
}

/* Shutdown pintos */
void halt (void) {
	power_off();
}

/* Exit process */
void exit(int status) {
    printf("%s: exit(%d)\n", thread_current()->name , status);
	thread_exit();
}

pid_t fork (const char *thread_name) {}

/* Create child process and execute program corresponds to cmd_file on it */
int exec (const char *cmd_line) {}

/* Wait for termination of child process whose process id is pid */
int wait (pid_t pid) {}

/* Create file which have size of initial_size. */
bool create (const char *file, unsigned initial_size) {}

/* Remove file whose name is file. */
bool remove (const char *file) {}

/* Open the file corresponds to path in "file". */
int open (const char *file) {}

/* Return the size, in bytes, of the file open as fd. */
int filesize (int fd) {}

int read (int fd, void *buffer, unsigned size) {}

int write(int fd, const void *buffer, unsigned size) {
	if(fd == 1) {
		putbuf(&buffer, size);
		return size;
	} else {
		printf("Let's write!\n");
		return size;
	}
}

void seek (int fd, unsigned position) {}
unsigned tell (int fd) {}
void close (int fd) {}