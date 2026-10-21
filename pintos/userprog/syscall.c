#include "userprog/syscall.h"
#include <stdio.h>
#include <limits.h>
#include "devices/input.h"
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
/* 사용자 페이지 테이블에서 메모리 주소의 매핑 여부를 확인하기 위한 헤더 */
#include "threads/mmu.h"
#include "threads/init.h"
#include "threads/loader.h"
#include "userprog/process.h"
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
/* 사용자 버퍼의 내용을 콘솔에 출력하기 위한 헤더 */
#include "kernel/console.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static void terminate (int status) NO_RETURN;
static void check_buffer (const void *buffer, unsigned size, bool writable);

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
/* 파일 시스템 동시 접근 보호: process.c의 ELF 로더와 공유 */
struct lock filesys_lock;

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
        pml4_get_page(curr->pml4, addr) == NULL ||
        !is_user_pte(pml4e_walk(curr->pml4, (uint64_t)addr, 0))) {

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

/* 사용자 버퍼의 전체 주소를 검사하고 잘못된 메모리 접근 시 프로세스를 종료하는 함수 */
static void
check_user_buffer(const void *buffer, unsigned size) {
    /* 시작 주소를 정수로 변환 */
    uintptr_t start = (uintptr_t)buffer;

    /* 크기가 0이면 검사 생략 */
    if(size == 0)
        return;

    /* 마지막 주소 계산 시 오버플로 검사 */
    if((uintptr_t)(size - 1) > UINTPTR_MAX - start) {
        thread_current()->exit_status = -1;
        thread_exit();
    }

    /* 버퍼에 포함된 모든 주소 검사 */
    for(uintptr_t i = 0; i < size; i++) {
        check_user_address((const void *)(start + i));
    }
}


/* 열린 파일에 사용 가능한 FD 번호를 할당 */
static int
allocate_fd(struct file *file) {
    struct thread *curr = thread_current();

    /* 0과 1은 표준 입출력용으로 예약 */
    for(int fd = 2; fd < FD_TABLE_SIZE; fd++) {

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
static struct file *
get_file (int fd) {
	struct file **table = thread_current ()->fd_table;
	if (fd < 2 || (unsigned) fd >= FD_TABLE_SIZE || table == NULL)
		return NULL;
	return table[fd];
}

static int
transfer (int fd, void *buffer, unsigned size, bool reading) {
	struct file *file = get_file (fd);
	if (file == NULL && fd != (reading ? 0 : 1))
		return -1;
	/* Empty requests never touch the buffer or advance the file position. */
	if (size == 0)
		return 0;
	check_buffer (buffer, size, reading);
	if (size > INT_MAX)
		return -1;
	if (reading && fd == 0) {
		for (unsigned i = 0; i < size; i++)
			((char *) buffer)[i] = input_getc ();
		return size;
	}
	if (!reading && fd == 1) {
		putbuf (buffer, size);
		return size;
	}
	lock_acquire (&filesys_lock);
	int count = reading ? file_read (file, buffer, size)
		: file_write (file, buffer, size);
	lock_release (&filesys_lock);
	return count;
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
    if(initial_size > INT_MAX) {
        f->R.rax = false;
        break;
    }

    /* 파일 생성 후 성공 여부 반환 */
    lock_acquire (&filesys_lock);
    bool success = filesys_create (name, initial_size);
    lock_release (&filesys_lock);

    f->R.rax = success;
    break;
}
    case SYS_REMOVE :{
        const char *name = (const char *) f->R.rdi;
        check_user_string (name);

        lock_acquire (&filesys_lock);
        bool success = filesys_remove (name);
        lock_release (&filesys_lock);

        f->R.rax = success;
        break;
    }
case SYS_OPEN: {
    /* 파일 이름 가져옴 */
     const char *name = (const char *)f->R.rdi;

     /* 파일 이름 전체 주소 검사 */
    check_user_string(name);

    /* 기존 파일 열기 */
	lock_acquire(&filesys_lock);
    struct file *file = filesys_open(name);
    lock_release(&filesys_lock);

    /* 파일이 없으면 실패 반환 */
    if(file == NULL) {
        f->R.rax = -1;
        break;
    }

    /* 열린 파일에 FD 번호 할당 */
    int fd = allocate_fd(file);

    /* FD 할당 실패 시 파일 닫기 */
    if(fd == -1) {
        lock_acquire(&filesys_lock);
        file_close(file);
        lock_release(&filesys_lock);
    }

    f->R.rax = fd;
    	break;
    }

        case SYS_WRITE: {
            /* 파일 번호와 버퍼 및 크기 가져옴 */
            int fd = (int)f->R.rdi;
            const void *buffer = (const void *)f->R.rsi;
            unsigned size = (unsigned)f->R.rdx;

    /* 기록할 데이터가 없으면 0 반환 */
    if(size == 0) {
        f->R.rax = 0;
        break;
    }

    /* 유효하지 않은 파일 번호 검사 */
    if(fd < 1 || fd >= FD_TABLE_SIZE ||
        (fd != 1 && thread_current()->fd_table[fd] == NULL)) {
        f->R.rax = -1;
         break;
    }

     /* 사용자 버퍼의 전체 메모리 범위 검사 */
    check_user_buffer(buffer, size);

	/* 표준 출력이면 콘솔에 기록 */
    if(fd == 1) {
    putbuf(buffer, size);
    f->R.rax = size;
        break;
}

    /* 일반 파일이면 실제 데이터 기록 */
    lock_acquire(&filesys_lock);
    f->R.rax = file_write(thread_current()->fd_table[fd], buffer, size);
    lock_release(&filesys_lock);

    break;
		}
		case SYS_READ:
			f->R.rax = transfer ((int) f->R.rdi, (void *) f->R.rsi,
					(unsigned) f->R.rdx, true);
			return;
		case SYS_CLOSE: {
			 uint64_t fd_Index = f->R.rdi;
			 if(fd_Index >= 2 && fd_Index < FD_TABLE_SIZE)
			 {
				if(thread_current()->fd_table[fd_Index] != NULL)
				{
					lock_acquire (&filesys_lock);
					file_close(thread_current()->fd_table[fd_Index]);
					thread_current()->fd_table[fd_Index] = NULL;
					lock_release (&filesys_lock);
				}
			 }
			return;
		}
    case SYS_HALT:
        power_off ();
        return;
    case SYS_EXIT:
        terminate ((int) f->R.rdi);
    case SYS_WAIT:
        f->R.rax = process_wait ((tid_t) f->R.rdi);
        return;
		case SYS_FILESIZE: {
			struct file *file = get_file ((int) f->R.rdi);
			if (file == NULL) {
				f->R.rax = -1;
				return;
			}
			lock_acquire (&filesys_lock);
			f->R.rax = file_length (file);
			lock_release (&filesys_lock);
			return;
		}
    /* SYS_SEEK */
    case SYS_SEEK:{
    uint64_t fd_Index = f->R.rdi;
    if(fd_Index >= 2 && fd_Index < FD_TABLE_SIZE)
    {
        if(thread_current()->fd_table[fd_Index] != NULL)
        {
            lock_acquire(&filesys_lock);
            file_seek(thread_current()->fd_table[fd_Index], f->R.rsi);
            lock_release(&filesys_lock);
        }
    }
	return;    
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

    if(curr->fd_table == NULL) return;

    lock_acquire(&filesys_lock);

    for(int fd = 2; fd < FD_TABLE_SIZE; fd++) {
        if(curr->fd_table[fd] != NULL) {
            file_close (curr->fd_table[fd]);
            curr->fd_table[fd] = NULL;
        }
    }

    lock_release(&filesys_lock);
}

static void
terminate (int status) {
    thread_current ()->exit_status = status;
    thread_exit ();
}

/* Validate before taking filesys_lock: invalid memory terminates the process. */
static void
check_buffer (const void *buffer, unsigned size, bool writable) {
	if (size == 0)
		return;
	uint64_t start = (uint64_t) buffer;
	if (start == 0 || start >= KERN_BASE || size - 1 > KERN_BASE - 1 - start)
		terminate (-1);
	uint64_t last = (uint64_t) pg_round_down (start + size - 1);
	for (uint64_t page = (uint64_t) pg_round_down (start);; page += PGSIZE) {
		uint64_t *pte = pml4e_walk (thread_current ()->pml4, page, 0);
		if (pte == NULL || !(*pte & PTE_P) || !is_user_pte (pte)
				|| (writable && !is_writable (pte)))
			terminate (-1);
		if (page == last)
			break;
	}
}
