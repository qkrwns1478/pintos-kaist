#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "threads/vaddr.h"

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
    printf("%s: exit(%d)\n", thread_current()->name, status);
	thread_exit();
}

/* Create new process which is the clone of current process with the name THREAD_NAME. */
pid_t fork (const char *thread_name) {}

/* Create child process and execute program corresponds to cmd_file on it. */
int exec (const char *cmd_line) {}

/* Wait for termination of child process whose process id is pid. */
int wait (pid_t pid) {}

/* Create file which have size of initial_size. */
bool create (const char *file, unsigned initial_size) {
	if (file == NULL) exit(-1); // return false;
	if (!is_user_vaddr(file)) exit(-1);
	return filesys_create(file, initial_size);
}

/* Remove file whose name is file. */
bool remove (const char *file) {
	return filesys_remove(file);
}

/* Open the file corresponds to path in "file". */
int open (const char *filename) {
	struct file *file = filesys_open(filename);
	if (file == NULL) return -1; // Return -1 if file is not opened
	struct thread *curr = thread_current();
	int fd;
	for(fd = 2; fd <= curr->next_fd; fd++) {
		if (curr->fdt[fd] == NULL) {
			curr->fdt[fd] = file;
			break;
		}
	}
	if (fd == curr->next_fd && curr->next_fd < 63) curr->next_fd++;
	else if (fd == curr->next_fd+1) {
		fd = -1; // Return -1 if the FDT is full
		file_close(file);
	}
	return fd;
}

/* Return the size, in bytes, of the file open as fd. */
int filesize (int fd) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct file *file = thread_current()->fdt[fd];
	if (file == NULL) return 0; // file not found
	return file_length(file);
}

/* Read size bytes from the file open as fd into buffer. */
int read (int fd, void *buffer, unsigned size) {
	if (fd == 0) return input_getc(); // fd0 is stdin
	else if (fd == 1) exit(-1); // fd1 is stdout
	else if (fd < 2 || fd > 63) exit(-1); // invalid fd
	else {
		struct file *file = thread_current()->fdt[fd];
		if (file == NULL) exit(-1);
		return file_read(file, buffer, size);
	}
}

/* Writes size bytes from buffer to the open file fd. */
int write(int fd, const void *buffer, unsigned size) {
	if(fd == 1) { // fd1 is stdout
		putbuf(buffer, size);
		return size;
	} else if (fd == 0) exit(-1); // fd0 is stdin
	else if (fd < 2 || fd > 63) exit(-1); // invalid fd
	else {
		struct file *file = thread_current()->fdt[fd];
		if (file == NULL) exit(-1);
		return file_write(file, buffer, size);
	}
}

/* Changes the next byte to be read or written in open file fd to position. */
void seek (int fd, unsigned position) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1); // file not found
	file_seek(file, position);
}

/* Return the position of the next byte to be read or written in open file fd. */
unsigned tell (int fd) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1); // file not found
	return file_tell(file);
}

/* Close file descriptor fd. */
void close (int fd) {
	if (fd < 2 || fd > 63) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1); // file not found
	curr->fdt[fd] = NULL;
	if (fd == curr->next_fd && curr->next_fd > 2) curr->next_fd--;
	file_close(file);
}