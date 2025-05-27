#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/interrupt.h"

void syscall_init (void);
void syscall_handler (struct intr_frame *);

int sys_write(int fd, const void *buffer, unsigned size);


#endif /* userprog/syscall.h */
