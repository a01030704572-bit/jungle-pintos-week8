#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태인 프로세스들의 목록, 즉 실행 준비는 되었으나
실제로 실행 중이지는 않은 프로세스들. */
static struct list ready_list;
//1차---------------------------------------------------------------------------------------------------------------------------------------------------
static struct list sleep_list;
//------------------------------------------------------------------------------------------------------------------------------------------------------

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드를 반환합니다.
 * CPU의 스택 포인터 `rsp'를 읽은 뒤, 이를 페이지 시작 주소로
 * 내림(round down)합니다. `struct thread'는 항상 페이지의
 * 시작 부분에 위치하고 스택 포인터는 그 중간 어딘가에 있으므로,
 * 이 과정을 통해 현재 스레드를 찾을 수 있습니다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 전역 기술자 테이블(GDT).
// GDT는 thread_init 이후에 설정되므로, 먼저 임시 GDT를 설정해야 합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
   일반적인 상황에서는 불가능한 작업이지만, loader.S에서 스택의
   바닥(bottom)을 페이지 경계에 맞추도록 세심하게 처리했기 때문에
   이 경우에는 가능합니다.

   또한 실행 큐(run queue)와 tid 잠금(lock)을 초기화합니다.

   이 함수를 호출한 후, thread_create()로 스레드를 생성하기 전에
   반드시 페이지 할당자(page allocator)를 초기화해야 합니다.

   이 함수가 완료되기 전에는 thread_current()를 호출하는 것이
   안전하지 않습니다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널을 위한 임시 GDT를 다시 로드합니다.
	 * 이 GDT에는 사용자 컨텍스트가 포함되지 않습니다.
	 * 커널은 gdt_init()에서 사용자 컨텍스트를 포함한 GDT를 다시 구성할 것입니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	//1차---------------------------------------------------------------------------------------------------------------------------------------------------
	list_init (&sleep_list);
	//------------------------------------------------------------------------------------------------------------------------------------------------------
	list_init (&destruction_req);

	/* 실행 중인 스레드를 위한 스레드 구조체를 설정합니다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
   또한 유휴(idle) 스레드를 생성합니다. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* 각 타이머 틱(timer tick)마다 타이머 인터럽트 핸들러에 의해 호출됩니다.
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 우선순위(PRIORITY)를 가진 NAME이라는 이름의 새로운 커널 스레드를 생성합니다.
   이 스레드는 AUX를 인자로 전달하여 FUNCTION을 실행하며, 준비 큐(ready queue)에 추가됩니다.
   새 스레드의 식별자(TID)를 반환하거나, 생성 실패 시 TID_ERROR를 반환합니다.

   thread_start()가 호출된 경우, 새 스레드는 thread_create()가 반환되기 전에 스케줄링될 수 있습니다.
   심지어 thread_create()가 반환되기 전에 종료될 수도 있습니다.
   반대로, 새 스레드가 스케줄링되기 전까지 원래 스레드가 얼마든지 계속 실행될 수도 있습니다.
   실행 순서를 보장해야 한다면 세마포어(semaphore)나 기타 동기화 기법을 사용하십시오.

   제공된 코드는 새 스레드의 `priority' 멤버를 PRIORITY로 설정하지만,
   실제 우선순위 스케줄링은 구현되어 있지 않습니다.
   우선순위 스케줄링 구현은 문제 1-3의 목표입니다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	return tid;
}

/* 현재 스레드를 대기(sleep) 상태로 만듭니다.
   thread_unblock()에 의해 깨어나기 전까지는 다시 스케줄링되지 않습니다.

   이 함수는 인터럽트가 비활성화된 상태에서 호출되어야 합니다.
   일반적으로는 synch.h에 정의된 동기화 프리미티브를 사용하는 편이 더 좋습니다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단된 스레드 T를 실행 가능한(ready-to-run) 상태로 전환합니다.
   T가 차단된 상태가 아니라면 오류입니다. (실행 중인 스레드를
   실행 가능한 상태로 만들려면 thread_yield()를 사용하십시오.)

   이 함수는 실행 중인 스레드를 선점(preempt)하지 않습니다.
   이는 중요한 점이 될 수 있습니다. 호출자가 직접 인터럽트를
   비활성화한 경우, 스레드의 차단을 해제하고 다른 데이터를
   업데이트하는 작업을 원자적(atomically)으로 수행할 수 있다고
   기대할 수 있기 때문입니다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_push_back (&ready_list, &t->elem);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

//1차----------------------------------------------------------------------------------------------------------------------------------
// 깨어날 시각(wakeup_tick)과 우선순위(priority)를 기준으로 스레드 순서를 비교
bool thread_wakeup_less(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
    struct thread *ta = list_entry(a, struct thread, elem);  // 첫 번째 리스트 요소에서 스레드 구조체를 가져옴
    struct thread *tb = list_entry(b, struct thread, elem);  // 두 번째 리스트 요소에서 스레드 구조체를 가져옴

    // 깨어날 시각이 서로 다르면
    if (ta->wakeup_tick < tb->wakeup_tick)
        return true;   // 첫 번째 스레드의 깨어날 시각이 더 빠르면 앞에 배치
    else if (ta->wakeup_tick > tb->wakeup_tick)
        return false;  // 첫 번째 스레드의 깨어날 시각이 더 늦으면 뒤에 배치

    // 깨어날 시각이 같고 우선순위가 다르면
    if (ta->priority > tb->priority)
        return true;   // 첫 번째 스레드의 우선순위가 더 높으면 앞에 배치
    else if (ta->priority < tb->priority)
        return false;  // 첫 번째 스레드의 우선순위가 더 낮으면 뒤에 배치

    // 깨어날 시각과 우선순위가 모두 같으면
    return false;      // 순서를 바꾸지 않음
}

void 
sleep_put(int64_t absolute_tick)			// 지정한 시각까지 현재 스레드를 잠들게 하는 함수
{
    struct thread *curr 	= thread_current();		// 현재 실행 중인 스레드의 정보를 가져옴  
    enum intr_level old_level;			 	// 인터럽트 상태를 저장할 변수
    old_level 			= intr_disable();		// 현재 인터럽트 상태를 저장하고 인터럽트를 비활성화
    curr->wakeup_tick 	= absolute_tick;	 	// 현재 스레드의 멤버 변수에 깨어날 시각을 저장
    list_insert_ordered(&sleep_list, &curr->elem, thread_wakeup_less, NULL);			// 현재 스레드를 추가
    thread_block();						// 현재 스레드를 BLOCKED 상태로 전환
    intr_set_level(old_level);				// 이전 인터럽트 상태로 복원
}

void
sleep_wakeup (int64_t current_tick) 
{
	struct list_elem *e = list_begin(&sleep_list);  	// 잠든 스레드 목록의 첫 번째 요소를 가져옴
	while (e != list_end(&sleep_list)) {             	// 목록의 끝에 도달할 때까지 반복
		struct thread *t = list_entry(e, struct thread, elem);  // 리스트 요소를 포함하는 스레드 구조체를 가져옴

		if (t->wakeup_tick <= current_tick) {       	// 깨어날 시간이 되었는지 확인
			e = list_remove(e);                     	// 목록에서 제거하고 다음 요소를 가리킴
			thread_unblock(t);                       	// 스레드를 실행 가능한 상태로 변경
		} else {	
			e = list_next(e);                        	// 다음 요소로 이동
		}
	}
}
//------------------------------------------------------------------------------------------------------------------------------------------------------

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 현재 실행 중인 스레드를 반환합니다.
   이는 running_thread()에 몇 가지 유효성 검사를 추가한 것입니다.
   자세한 내용은 thread.h 상단의 상세 주석을 참조하십시오. */struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

/* T가 실제로 스레드인지 확인합니다.
	   이 단언문(assertion) 중 하나라도 실패한다면, 해당 스레드의
	   스택이 오버플로우되었을 수 있습니다. 각 스레드는 4kB 미만의
	   스택을 가지므로, 크기가 큰 자동 배열을 몇 개 사용하거나
	   적당한 수준의 재귀 호출만으로도 스택 오버플로우가 발생할 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_push_back (&ready_list, &curr->elem);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* 다음에 스케줄링할 스레드를 선택하여 반환합니다. 
실행 큐(run queue)가 비어 있지 않다면 실행 큐에서 스레드를 반환해야 합니다. 
(현재 실행 중인 스레드가 계속 실행될 수 있는 상태라면,
해당 스레드도 실행 큐에 포함되어 있을 것입니다.)
실행 큐가 비어 있다면 idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
/* 전환하기 전의 스레드가 종료되는 중이라면 해당 스레드의 구조체(struct thread)를 파괴해야 합니다. 
단, thread_exit()가 실행 도중 자신의 기반을 무너뜨리는 상황을 피하기 위해 이 작업은 마지막 단계에서 수행되어야 합니다. 
현재 해당 페이지가 스택으로 사용 중이므로, 여기서는 페이지 해제 요청을 큐에 넣기만 합니다. 
실제 파괴 로직은 schedule()의 시작 부분에서 호출될 것입니다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
