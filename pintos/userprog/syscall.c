#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "threads/loader.h"
#include "userprog/process.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

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
	// TODO: Your implementation goes here.
	switch (f->R.rax) {
		case SYS_OPEN:
			if(f->R.rdi != 0 && is_user_vaddr((const char *)f->R.rdi))
			{
				void *page = pml4_get_page (thread_current()->pml4, (const char *)f->R.rdi);
				if(page == NULL)
				{
					f->R.rax = -1;
					if (thread_current ()->child_status != NULL)
						thread_current ()->child_status->exit_status = (int) f->R.rax;
					thread_exit ();
				}

				const char *rdiChar = (const char *)f->R.rdi;
				for(int i = 0;; i++)
				{
					if(!is_user_vaddr(rdiChar + i))
					{
						f->R.rax = -1;
						if (thread_current ()->child_status != NULL)
							thread_current ()->child_status->exit_status = (int) f->R.rax;
						thread_exit ();
					}
					else 
					{
						void *page2 = pml4_get_page (thread_current()->pml4, rdiChar + i);
						if(page2 == NULL)
						{
							f->R.rax = -1;
							if (thread_current ()->child_status != NULL)
								thread_current ()->child_status->exit_status = (int) f->R.rax;
							thread_exit ();
						}
						if(rdiChar[i] == '\0') break;
					}
				}
				struct file *file = filesys_open(rdiChar);
				if (file != NULL)
				{
					bool isSave=false;
					for(int i = 2; i<FD_TABLE_SIZE;i++)
					{
						if(thread_current()->fd_Table[i] == NULL)
						{	
							thread_current()->fd_Table[i] = file;
							f->R.rax = i;
							isSave = true;
							break;
						}							
					}					
					if(!isSave)
					{
						file_close(file);
						f->R.rax = -1;
					}
					return;
				}
				f->R.rax = -1;
				return;
			}
			f->R.rax = -1;
			if (thread_current ()->child_status != NULL)
				thread_current ()->child_status->exit_status = (int) f->R.rax;
			thread_exit ();
			break;
		case SYS_HALT:
			power_off ();
			break;
		case SYS_EXIT:
			if (thread_current ()->child_status != NULL)
				thread_current ()->child_status->exit_status = (int) f->R.rdi;
			thread_exit ();
		case SYS_WAIT:
			f->R.rax = process_wait ((tid_t) f->R.rdi);
			return;
		case SYS_WRITE:
			if(f->R.rdi == 1)
			{
				// 출력할 바이트가 없을 때
				if (f->R.rdx == 0) {
					f->R.rax = 0;
					return;
				}

				// 마지막 바이트 주소(buffer + size - 1)의 오버플로 검사
				if (f->R.rdx - 1 > UINT64_MAX - f->R.rsi) {
					if (thread_current ()->child_status != NULL)
						thread_current ()->child_status->exit_status = -1;
					thread_exit ();
				}
				uint64_t start = pg_round_down(f->R.rsi);
				uint64_t last = pg_round_down(f->R.rsi + f->R.rdx - 1);

				bool isvalid = true;
				// 모든 페이지 검사
				for (;start <= last; start += PGSIZE)
				{
					isvalid = is_user_vaddr((const char *)start);
					if(!isvalid) break;

					void *parent_page = pml4_get_page (thread_current()->pml4, (const char *)start);
					if(parent_page == NULL)
					{
						isvalid = false;
						break;
					}
				}
				// 출력한 바이트 수 반환
				if (isvalid)
				{
					putbuf((const char *)f->R.rsi, f->R.rdx);
					f->R.rax = f->R.rdx;
					return;
				}				
			}

			f->R.rax = -1;
			if (thread_current ()->child_status != NULL)
				thread_current ()->child_status->exit_status = (int) f->R.rax;
			thread_exit ();
			return;
		default:
			break;
	}
	printf ("system call!\n");
	thread_exit ();
}
