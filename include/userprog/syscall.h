#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include "threads/interrupt.h"
typedef int pid_t;
struct lock filesys_lock;

void syscall_init (void);

void halt (void);
void exit(int status);
pid_t fork (const char *thread_name, struct intr_frame *f);
int exec (const char *cmd_line);
int wait (pid_t pid);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *filename);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
int write (int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

#ifdef VM
#include <stddef.h>
#include "filesys/off_t.h"
void *mmap (void *addr, size_t length, int writable, int fd, off_t offset);
void munmap (void *addr);
#endif

#endif /* userprog/syscall.h */
