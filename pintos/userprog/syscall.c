#include "userprog/syscall.h"
#include <stdio.h>
#include <limits.h>
#include "filesys/filesys.h"
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/init.h"
#include "threads/loader.h"
#include "userprog/process.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

struct lock filesys_lock;

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
	lock_init (&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

static void
terminate (int status) {
	if (thread_current ()->child_status != NULL)
		thread_current ()->child_status->exit_status = status;
	thread_exit ();
}

/* Validate before taking filesys_lock: invalid memory terminates the process. */
static void
check_buffer (const void *buffer, unsigned size) {
	if (size == 0)
		return;
	uint64_t start = (uint64_t) buffer;
	if (start == 0 || start >= KERN_BASE || size - 1 > KERN_BASE - 1 - start)
		terminate (-1);
	uint64_t last = (uint64_t) pg_round_down (start + size - 1);
	for (uint64_t page = (uint64_t) pg_round_down (start);; page += PGSIZE) {
		uint64_t *pte = pml4e_walk (thread_current ()->pml4, page, 0);
		if (pte == NULL || !(*pte & PTE_P) || !is_user_pte (pte))
			terminate (-1);
		if (page == last)
			break;
	}
}

static void
check_string (const char *name) {
	check_buffer (name, 1);
	while (*name != '\0') {
		name++;
		if (pg_ofs (name) == 0)
			check_buffer (name, 1);
	}
}

/* Console output is the only write service needed by this submission. */
static int
write_console (int fd, const void *buffer, unsigned size) {
	if (fd != 1)
		return -1;
	if (size == 0)
		return 0;
	check_buffer (buffer, size);
	if (size > INT_MAX)
		return -1;
	putbuf (buffer, size);
	return size;
}

/* Register arguments follow the x86-64 syscall ABI. */
void
syscall_handler (struct intr_frame *f) {
	switch (f->R.rax) {
		case SYS_HALT:
			power_off ();
			return;
		case SYS_EXIT:
			terminate ((int) f->R.rdi);
			return;
		case SYS_WAIT:
			f->R.rax = process_wait ((tid_t) f->R.rdi);
			return;
		case SYS_CREATE:
			check_string ((const char *) f->R.rdi);
			if ((unsigned) f->R.rsi > INT_MAX) {
				f->R.rax = false;
				return;
			}
			lock_acquire (&filesys_lock);
			f->R.rax = filesys_create ((const char *) f->R.rdi, (unsigned) f->R.rsi);
			lock_release (&filesys_lock);
			return;
		case SYS_WRITE:
			f->R.rax = write_console ((int) f->R.rdi, (const void *) f->R.rsi,
					(unsigned) f->R.rdx);
			return;
		default:
			terminate (-1);
	}
}
