#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
void refresh_priority(void);
void donate_priority(void);
void remove_with_lock(struct lock *lock);
void donate_priority_thread(struct thread *t);
bool compare_thread_priority(const struct list_elem *a_, const struct list_elem *b_, void *aux);

/* Alarm clock */
bool cmp_wakeup_tick(const struct list_elem *a_, const struct list_elem *b_, void *aux);


#endif /* userprog/process.h */
