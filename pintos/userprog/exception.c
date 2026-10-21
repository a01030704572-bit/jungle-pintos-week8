#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) {
	/* These exceptions can be raised explicitly by a user program,
	   e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
	   we set DPL==3, meaning that user programs are allowed to
	   invoke them via these instructions. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* These exceptions have DPL==0, preventing user processes from
	   invoking them via the INT instruction.  They can still be
	   caused indirectly, e.g. #DE can be caused by dividing by
	   0.  */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* Most exceptions can be handled with interrupts turned on.
	   We need to disable interrupts for page faults because the
	   fault address is stored in CR2 and needs to be preserved. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 (아마도) 사용자 프로세스에 의해 발생한 것일 수 있습니다.
   예를 들어, 프로세스가 매핑되지 않은 가상 메모리에 접근하려 했을 경우(페이지 폴트)가 이에 해당합니다.
   현재로서는 단순히 해당 사용자 프로세스를 종료(kill)합니다.
   추후에는 커널 차원에서 페이지 폴트를 처리하도록 구현할 예정입니다.
   실제 유닉스 계열 운영체제에서는 대부분의 예외를 시그널(signal)을 통해 프로세스로 전달하지만,
   현재 우리 시스템에는 시그널 기능이 구현되어 있지 않습니다. */

	/* The interrupt frame's code segment value tells us where the
	   exception originated. */
	switch (f->cs) {
		case SEL_UCSEG:
     /* 사용자 예외 발생 시 종료 상태를 -1로 설정 */
    thread_current()->exit_status = -1;

    /* 현재 사용자 프로세스 종료 */
    thread_exit ();
			// printf ("%s: dying due to interrupt %#04llx (%s).\n",
			// 		thread_name (), f->vec_no, intr_name (f->vec_no));
			// intr_dump_frame (f);
			// thread_exit ();

		case SEL_KCSEG:
			/* Kernel's code segment, which indicates a kernel bug.
			   Kernel code shouldn't throw exceptions.  (Page faults
			   may cause kernel exceptions--but they shouldn't arrive
			   here.)  Panic the kernel to make the point.  */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* Some other code segment?  Shouldn't happen.  Panic the
			   kernel. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* 페이지 폴트 핸들러(Page fault handler)입니다. 
가상 메모리를 구현하기 위해 내용을 채워 넣어야 하는 기본 틀(skeleton)입니다. 
프로젝트 2의 일부 해결 방안에서는 이 코드를 수정해야 할 수도 있습니다.
핸들러가 호출될 때, 페이지 폴트가 발생한 주소는 CR2(Control Register 2) 레지스터에 저장되며, 
폴트와 관련된 정보는 `exception.h` 파일 내 `PF_*` 매크로에 정의된 형식으로 `f->error_code` 멤버에 담겨 전달됩니다. 
여기에 제시된 예제 코드는 해당 정보를 어떻게 해석(파싱)하는지 보여줍니다. 
이와 관련된 더 자세한 내용은 [IA32-v3a] 문서의 5.15절 "Exception and Interrupt Reference"에 있는 
"Interrupt 14--Page Fault Exception (#PF)" 항목에서 확인할 수 있습니다. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* True: not-present page, false: writing r/o page. */
	bool write;        /* True: access was write, false: access was read. */
	bool user;         /* True: access by user, false: access by kernel. */
	void *fault_addr;  /* Fault address. */

	/* Obtain faulting address, the virtual address that was
	   accessed to cause the fault.  It may point to code or to
	   data.  It is not necessarily the address of the instruction
	   that caused the fault (that's f->rip). */

	fault_addr = (void *) rcr2();

	/* 인터럽트 켜기 */
	intr_enable ();

	/* Determine cause. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* For project 3 and later. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* Count page faults. */
	page_fault_cnt++;

	/* 사용자 예외는 kill에서 -1로 종료하고 커널 오류 진단은 유지한다. */
	if (user)
		kill (f);

	/* If the fault is true fault, show info and exit. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

