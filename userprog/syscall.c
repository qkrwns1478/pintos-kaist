#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool check_address (const void *addr);

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
	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
// #ifdef VM
//     thread_current()->stack_pointer = f->rsp;
// #endif
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
			f->R.rax = exec(f->R.rdi);
			break;
		case SYS_WAIT:                   /* Wait for a child process to die. */
			f->R.rax = wait(f->R.rdi);
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
#ifdef VM
		case SYS_MMAP:					 /* Map a file into memory. */
			f->R.rax = mmap(f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
			break;
		case SYS_MUNMAP:				 /* Remove a memory mapping. */
			munmap(f->R.rdi);
			break;
#endif
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
	if (curr->running_file)
		file_allow_write(curr->running_file);
	thread_exit();
}

/* Create new process which is the clone of current process with the name THREAD_NAME. */
pid_t fork (const char *thread_name, struct intr_frame *f) {
	if (!check_address(thread_name)) exit(-1);
	return process_fork(thread_name, f);
}

/* Create child process and execute program corresponds to cmd_file on it. */
int exec (const char *cmd_line) {
	if (!check_address(cmd_line)) exit(-1);
	char *buf = palloc_get_page(PAL_ZERO);
	if (buf == NULL) exit(-1);
	strlcpy(buf, cmd_line, PGSIZE);
	if (process_exec(buf) == -1) exit(-1);
	NOT_REACHED();
}

/* Wait for termination of child process whose process id is pid. */
int wait (pid_t pid) {
	return process_wait(pid);
}

/* Create file which have size of initial_size. */
bool create (const char *file, unsigned initial_size) {
	if (!check_address(file)) exit(-1);
	lock_acquire(&filesys_lock);
	bool res = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	return res;
}

/* Remove file whose name is file. */
bool remove (const char *file) {
	if (!check_address(file)) exit(-1);
	lock_acquire(&filesys_lock);
	bool res = filesys_remove(file);
	lock_release(&filesys_lock);
	return res;
}

/* Open the file corresponds to path in "file". */
int open (const char *filename) {
	if (!check_address(filename)) exit(-1);
	struct thread *curr = thread_current();
	int fd;
	bool is_not_full = false;
	for(fd = 2; fd < FILED_MAX; fd++) {
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
	if (fd < 2 || fd >= FILED_MAX) exit(-1); // invalid fd
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
	if (!check_address(buffer)) exit(-1);
#ifdef VM
    struct page *page = spt_find_page(&thread_current()->spt, buffer);
    if (page != NULL && !page->writable)
        exit(-1);
#endif
	if (fd == 0) { // fd0 is stdin
		char *buf = (char *) buffer;
		for (int i = 0; i < size; i++) {
			buf[i] = input_getc();
		}
		return size;
	}
	else if (fd == 1) exit(-1); // fd1 is stdout (invalid)
	else if (fd < 2 || fd >= FILED_MAX) exit(-1); // invalid fd
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
	if (!check_address(buffer)) exit(-1);
	if(fd == 1) { // fd1 is stdout
		putbuf(buffer, size);
		return size;
	} else if (fd == 0) exit(-1); // fd0 is stdin (invalid)
	else if (fd < 2 || fd >= FILED_MAX) exit(-1); // invalid fd
	else {
		struct thread *curr = thread_current();
		struct file *file = curr->fdt[fd];
		if (file == NULL) exit(-1);
		if (curr->running_file == file) return 0;
		lock_acquire(&filesys_lock);
		off_t res = file_write(file, buffer, size);
		lock_release(&filesys_lock);
		if (res < 0) return -1;
		return res;
	}
}

/* Changes the next byte to be read or written in open file fd to position. */
void seek (int fd, unsigned position) {
	if (fd < 2 || fd >= FILED_MAX) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1);
	lock_acquire(&filesys_lock);
	file_seek(file, position);
	lock_release(&filesys_lock);
}

/* Return the position of the next byte to be read or written in open file fd. */
unsigned tell (int fd) {
	if (fd < 2 || fd >= FILED_MAX) exit(-1); // invalid fd
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
	if (fd < 2 || fd >= FILED_MAX) exit(-1); // invalid fd
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];
	if (file == NULL) exit(-1);
	curr->fdt[fd] = NULL;
	// if (fd == curr->next_fd && curr->next_fd > 2) curr->next_fd--;
	if (curr->running_file == file)
		curr->running_file = NULL;
	lock_acquire(&filesys_lock);
	file_close(file);
	lock_release(&filesys_lock);
}

#ifdef VM

/* Load file data into memory. */
void *mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
	if (length == 0 || pg_round_down(addr) != addr || pg_round_down(offset) != offset || addr == 0 || fd == 0 || fd == 1)
		return NULL;
	struct file *file = thread_current()->fdt[fd];
	if (file == NULL || file_length(file) == 0)
		return NULL;
	return do_mmap(addr, length, writable, file, offset);
}

/* Unmap the mappings which has not been previously unmapped. */
void munmap (void *addr) {
	if (pg_round_down(addr) != addr || addr == 0)
		return;
	do_munmap(addr);
}

#endif

bool check_address(const void *addr) {
	if (addr == NULL || !is_user_vaddr(addr)) 
		return false;
#ifndef VM
	void *page = pml4_get_page(thread_current()->pml4, addr);
#else
	struct page *page = spt_find_page(&thread_current()->spt, addr);
#endif
	if (page == NULL)
		return false;
	return true;
}