#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */
/* 커널 스레드 또는 사용자 프로세스.
 *
 * 각 스레드 구조체는 4KB 크기의 전용 페이지에 저장됩니다.
 * 스레드 구조체 자체는 페이지의 맨 아래쪽(오프셋 0)에 위치합니다.
 * 페이지의 나머지 공간은 스레드의 커널 스택을 위해 할당되며,
 * 이 스택은 페이지의 맨 위쪽(오프셋 4KB)에서 아래 방향으로 자라납니다.
 * 다음은 이에 대한 그림입니다:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이로 인한 결과는 크게 두 가지입니다:
 *
 *    1. 첫째, `struct thread'의 크기가 너무 커지지 않도록 해야 합니다.
 *       크기가 너무 커지면 커널 스택을 위한 공간이 부족해질 수 있습니다.
 *       기본 `struct thread'의 크기는 불과 몇 바이트에 불과합니다.
 *       가급적 1kB 미만으로 유지하는 것이 좋습니다.
 *
 *    2. 둘째, 커널 스택의 크기가 너무 커지지 않도록 해야 합니다.
 *       스택이 넘치면(overflow) 스레드 상태가 손상될 수 있습니다.
 *       따라서 커널 함수 내에서 큰 구조체나 배열을 static이 아닌
 *       지역 변수로 할당해서는 안 됩니다. 대신 malloc()이나
 *       palloc_get_page()를 사용한 동적 할당을 이용하십시오.
 *
 * 이러한 문제들이 발생했을 때 나타나는 첫 번째 증상은 아마도 thread_current()에서의 assertion 실패일 것입니다. 이 함수는
 * 현재 실행 중인 스레드의 `struct thread'에 있는 `magic' 멤버가 * THREAD_MAGIC으로 설정되어 있는지 확인합니다. 스택 오버플로우가
 * 발생하면 대개 이 값이 변경되어 assertion 실패를 유발합니다. */

/* `elem' 멤버는 두 가지 용도로 사용됩니다. 실행 큐(run queue, thread.c)의 요소가 될 수도 있고,
 * 세마포어 대기 목록(semaphore wait list, synch.c)의 요소가 * 될 수도 있습니다. 이 두 가지 용도로 사용 가능한 이유는
 * 이들이 상호 배타적(mutually exclusive)이기 때문입니다. * 즉, 준비(ready) 상태의 스레드만 실행 큐에 존재하고,
 * 반면 차단(blocked) 상태의 스레드만 세마포어 대기 목록에 존재하기 때문입니다. */
 
struct thread {
	/*thread.c가 소유함. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	char name[16];                      /* 이름 (디버깅용). */
	int priority;                       /* 우선 사항. */

	/* thread.c와 synch.c 간에 공유됩니다. */
	struct list_elem elem;             		/*목록 요소. */

//1차---------------------------------------------------------------------------------------------------------------------------------------------------
	/* Alarm clock: 일어날 시각 (timer tick 단위) */
	int64_t wakeup_tick;					 // 스레드가 깨어나야 하는 시각을 타이머 틱 단위로 저장
//------------------------------------------------------------------------------------------------------------------------------------------------------

	#ifdef USERPROG
		/*userprog/process.c가 소유함. */
		uint64_t *pml4;                     /* 페이지 맵 레벨 4 */
	#endif
	#ifdef VM
		/* 스레드가 소유한 전체 가상 메모리에 대한 테이블. */
		struct supplemental_page_table spt;
	#endif

		/* Owned by thread.c. */
		struct intr_frame tf;               /* 전환 관련 정보 */
		unsigned magic;                     /* 스택 오버플로를 감지합니다. */

};

/* false(기본값)인 경우 라운드 로빈 스케줄러를 사용합니다. 
true인 경우 다단계 피드백 큐 스케줄러를 사용합니다. 
커널 명령줄 옵션 "-o mlfqs"로 제어됩니다. */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);
//1차---------------------------------------------------------------------------------------------------------------------------------------------------
void sleep_put 		(int64_t);
void sleep_wakeup 	(int64_t);
//------------------------------------------------------------------------------------------------------------------------------------------------------

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */
