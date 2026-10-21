#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

extern struct lock filesys_lock;
void syscall_init (void);
void syscall_close_all_files (void);

#endif /* userprog/syscall.h */
