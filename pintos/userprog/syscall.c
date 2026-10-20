#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
/* 파일 생성과 열기에 필요한 헤더 */
#include "filesys/filesys.h"
#include "filesys/file.h"

/* 파일 시스템 동시 접근 제어 */
#include "threads/synch.h"

/* 사용자 메모리 주소 검사 */
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
/* 파일 시스템 동시 접근 보호 */
static struct lock filesys_lock;

void
syscall_init (void) {
	/* 파일 시스템 잠금 초기화 */
	lock_init(&filesys_lock);

	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 사용자 메모리 주소가 정상적으로 접근 가능한지 검사 */
static void
check_user_address(const void *addr) {
    struct thread *curr = thread_current ();

    /* 잘못된 사용자 주소인지 검사 */
    if(addr == NULL ||
        !is_user_vaddr(addr) ||
        pml4_get_page(curr->pml4, addr) == NULL) {

        /* 비정상 종료 상태 설정 */
        curr->exit_status = -1;
        thread_exit();
    }
}

/* 파일 이름 문자열을 처음부터 끝까지 검사 */
static void
check_user_string(const char *str) {
    const char *p = str;

    while(true) {
        /* 현재 문자 주소 검사 */
        check_user_address (p);

        /* 문자열 끝에 도달하면 반복 종료 */
        if(*p == '\0')
            break;

        /* 다음 문자로 이동 */
        p++;
    }
}

/* 열린 파일에 사용 가능한 FD 번호를 할당 */
static int
allocate_fd(struct file *file) {
    struct thread *curr = thread_current();

    /* 0과 1은 표준 입출력용으로 예약 */
    for(int fd = 2; fd < 128; fd++) {

        /* 사용하지 않는 FD 번호 확인 */
        if(curr->fd_table[fd] == NULL) {

            /* 해당 FD 번호에 파일 등록 */
            curr->fd_table[fd] = file;

            /* 할당한 FD 번호 반환 */
            return fd;
        }
    }

    /* 사용 가능한 FD 번호가 없는 경우 */
    return -1;
}
/* The main system call interface */
void
syscall_handler(struct intr_frame *f) {
	// TODO: Your implementation goes here.
	 /* 시스템 콜 번호 */
    int syscall_number = f->R.rax;
	switch (syscall_number) {
    case SYS_CREATE: {
    /* 파일 이름과 초기 크기 가져옴 */
    const char *name = (const char *) f->R.rdi;
    unsigned initial_size = (unsigned) f->R.rsi;

    /* 파일 이름 전체의 주소 검사 */
    check_user_string (name);

    /* 파일 생성 후 성공 여부 반환 */
    lock_acquire (&filesys_lock);
    bool success = filesys_create (name, initial_size);
    lock_release (&filesys_lock);

    f->R.rax = success;
    break;
}

case SYS_OPEN: {
    /* 파일 이름 가져옴 */
    const char *name = (const char *) f->R.rdi;

    /* 파일 이름 전체의 주소 검사 */
    check_user_string (name);

    /* 기존 파일 열기 */
    lock_acquire(&filesys_lock);
    struct file *file = filesys_open(name);
    lock_release(&filesys_lock);

    /* 파일이 존재하지 않으면 실패 */
    if(file == NULL) {
        f->R.rax = -1;
        break;
    }

    /* 열린 파일에 FD 번호 할당 */
    int fd = allocate_fd(file);

    /* FD 할당 실패 시 파일 닫기 */
    if(fd == -1) {
        lock_acquire (&filesys_lock);
        file_close (file);
        lock_release (&filesys_lock);
    }

    f->R.rax = fd;
    break;
		}
    /* 지원하지 않는 시스템 콜에 실패값을 반환하는 처리 */
	default:
    /* 처리할 기능이 없으면 -1 반환 */
    f->R.rax = -1;
    break;
	// printf ("system call!\n");
	// thread_exit ();
	}
}

/* 현재 프로세스가 연 파일 전부 닫음 */
void
syscall_close_all_files(void) {
    struct thread *curr = thread_current();

    lock_acquire(&filesys_lock);

    for(int fd = 2; fd < 128; fd++) {
        if(curr->fd_table[fd] != NULL) {
            file_close (curr->fd_table[fd]);
            curr->fd_table[fd] = NULL;
        }
    }

    lock_release(&filesys_lock);
}
