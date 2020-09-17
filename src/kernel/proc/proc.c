#include <list.h>
#include <proc/sched.h>
#include <mem/malloc.h>
#include <proc/proc.h>
#include <ssulib.h>
#include <interrupt.h>
#include <proc/sched.h>
#include <syscall.h>
#include <mem/palloc.h>
#include <string.h>
#include <device/io.h>
#include <device/console.h>
#include <mem/paging.h>

#define STACK_SIZE 512

struct list plist;				// All Process List
struct list slist;				// Sleep Process List
struct list rlist;				// Running Process List
struct list runq[RQ_NQS];		// Priority array

struct process procs[PROC_NUM_MAX];
struct process *cur_process;
struct process *idle_process;
int pid_num_max;
uint32_t process_stack_ofs;
static int lock_pid_simple; 
static int lately_pid;

bool more_prio(const struct list_elem *a, const struct list_elem *b, void *aux);
bool less_time_sleep(const struct list_elem *a, const struct list_elem *b, void *aux);
pid_t getValidPid(int *idx);
void proc_start(void);
void proc_end(void);
void proc_getpos(int, int *);	

void kernel1_proc(void *aux);
void kernel2_proc(void *aux);
void kernel3_proc(void *aux);

/*
Hint : use 'printk' function to trace function call
*/

void init_proc()
{
	int i;
	process_stack_ofs = offsetof (struct process, stack);

	lock_pid_simple = 0;
	lately_pid = -1;

	list_init(&plist);
	list_init(&slist);
  
    // runq를 초기화 한다. 
    for (int x =0; x< RQ_NQS; x++) {
        list_init(&runq[x]);
    }
    for (i = 0; i < PROC_NUM_MAX; i++)
	{
		procs[i].pid = i;
		procs[i].state = PROC_UNUSED;
		procs[i].parent = NULL;
	}

	pid_t pid = getValidPid(&i);
	cur_process = &procs[0];
	idle_process = &procs[0];

	cur_process->pid = pid;
	cur_process->parent = NULL;
	cur_process->state = PROC_RUN;

	cur_process -> priority = 99;

	cur_process->stack = 0;
	cur_process->pd = (void*)read_cr3();
	cur_process -> elem_all.prev = NULL;
	cur_process -> elem_all.next = NULL;
	cur_process -> elem_stat.prev = NULL;
	cur_process -> elem_stat.next = NULL;

   	list_push_back(&plist, &cur_process->elem_all);
    //0번 프로세스는 priority 값이 99 이므로 runq 에 index를 계산하여 넣는다. 
    int idx;
    proc_getpos(cur_process->priority,&idx);
    list_push_back(&runq[idx],&cur_process->elem_stat);
}

pid_t getValidPid(int *idx) {
	pid_t pid = -1;
	int i;

	while(lock_pid_simple);

	lock_pid_simple++;

	for(i = 0; i < PROC_NUM_MAX; i++)
	{
		int tmp = i + lately_pid + 1;
		if(procs[tmp % PROC_NUM_MAX].state == PROC_UNUSED) { 
			pid = lately_pid + 1;
			*idx = tmp % PROC_NUM_MAX;
			break;
		}
	}

	if(pid != -1)
		lately_pid = pid;	

	lock_pid_simple = 0;

	return pid;
}

pid_t proc_create(proc_func func, struct proc_option *opt, void* aux)
{
	struct process *p;
	int idx;

	enum intr_level old_level = intr_disable();

	pid_t pid = getValidPid(&idx);
	p = &procs[pid];
	p->pid = pid;
	p->state = PROC_RUN;

	if(opt != NULL) 
		p -> priority = opt -> priority;
	else 
		p -> priority = (unsigned char)99;

	p->time_used = 0;
	p->time_slice = 0;
	p->parent = cur_process;
	p->simple_lock = 0;
	p->child_pid = -1;
	p->pd = pd_create(p->pid);

	//init stack
	int *top = (int*)palloc_get_page();
	int stack = (int)top;
	top = (int*)stack + STACK_SIZE - 1;

	*(--top) = (int)aux;		//argument for func
	*(--top) = (int)proc_end;	//return address from func
	*(--top) = (int)func;		//return address from proc_start
	*(--top) = (int)proc_start; //return address from switch_process

	//process call stack : 
	//switch_process > proc_start > func(aux) > proc_end

	*(--top) = (int)((int*)stack + STACK_SIZE - 1); //ebp
	*(--top) = 1; //eax
	*(--top) = 2; //ebx
	*(--top) = 3; //ecx
	*(--top) = 4; //edx
	*(--top) = 5; //esi
	*(--top) = 6; //edi

	p -> stack = top;
	p -> elem_all.prev = NULL;
	p -> elem_all.next = NULL;
	p -> elem_stat.prev = NULL;
	p -> elem_stat.next = NULL;
	list_push_back(&plist, &p->elem_all);//전체 인덱스는 plist에
   
    //생성한 proc을runq 에 삽입 한다. 
    proc_getpos(p->priority, &idx);
    list_push_back(&runq[idx],&p->elem_stat);
    
	intr_set_level (old_level);
	return p->pid;
}

void* getEIP()
{
    return __builtin_return_address(0);
}
//priority를 재계산 한다. 
void recalculate_priority(void)
{
	int idx;
	struct process *p = cur_process;
    if (cur_process->pid ==0) 
        ; //현재 프로세스가 idle일 경우 계산하지 않는다. 
    else { // 0이 아닌 경우
    
    list_remove(&cur_process->elem_stat); //현재의 runq에서 제거한다. 

    p->priority += p->time_slice / 10;
    proc_getpos(p->priority,&idx);
    list_push_back(&runq[idx],&p->elem_stat); 
    //계산 후 해당하는 인덱스의 리스트 맨 마지막에 삽입한다. 
    }
}

void  proc_start(void)
{
	intr_enable ();
	return;
}

void proc_free(void)
{
	uint32_t pt = *(uint32_t*)cur_process->pd;
	cur_process->parent->child_pid = cur_process->pid;
	cur_process->parent->simple_lock = 0;

	cur_process->state = PROC_ZOMBIE;

	palloc_free_page(cur_process->stack);
	palloc_free_page((void*)pt);
	palloc_free_page(cur_process->pd);

	list_remove(&cur_process->elem_stat);
	list_remove(&cur_process->elem_all);
}

void proc_end(void)
{
	proc_free();
	schedule();
	return;
}

void proc_wake(void)
{
	struct process* p;
	int idx;
	int old_level;
	unsigned long long t = get_ticks();

    while(!list_empty(&slist))
	{
		p = list_entry(list_front(&slist), struct process, elem_stat);
         
		if(p->time_sleep > t)
			break;
        //현재의 list에서 제거한다.  
		list_remove(&p->elem_stat);
        //다시runq에 삽입한다. 
        proc_getpos(p->priority,&idx);
        list_push_back(&runq[idx], &p->elem_stat);
        p->state = PROC_RUN;
	}
}

void proc_sleep(unsigned ticks)
{
    unsigned long cur_ticks = get_ticks();

        //현재 time_sleep의값을 입력받은 tick과 현재 tick을 더한다. 
	cur_process->time_sleep = cur_ticks+ ticks;
	cur_process->state = PROC_STOP;
	cur_process->time_slice = 0;
   
    //현재의 list 에서 제거한다. 
    list_remove(&cur_process->elem_stat);
    //slist에 삽입한다. 
    list_insert_ordered(&slist, &cur_process->elem_stat,
                        less_time_sleep, NULL);
    schedule();
}

void proc_block(void)
{
	cur_process->state = PROC_BLOCK;
	schedule();	
}

void proc_unblock(struct process* proc)
{
	enum intr_level old_level;
	int idx;
    //해당하는 idx에 프로세스를 삽입한다. 
    proc_getpos(proc->priority,&idx);
	list_push_back(&runq[idx], &proc->elem_stat);
	proc->state = PROC_RUN;
}     

bool less_time_sleep(const struct list_elem *a, const struct list_elem *b,void *aux)
{
	struct process *p1 = list_entry(a, struct process, elem_stat);
	struct process *p2 = list_entry(b, struct process, elem_stat);

	return p1->time_sleep < p2->time_sleep;
}

bool more_prio(const struct list_elem *a, const struct list_elem *b,void *aux)
{
	struct process *p1 = list_entry(a, struct process, elem_stat);
	struct process *p2 = list_entry(b, struct process, elem_stat);
	
	return p1->priority > p2->priority;
}

void kernel1_proc(void* aux)
{
	int passed = 0;
    while(1)
	{    
        //intr_disable , enable 로 출력에 의한 오차를 줄인다.        
        intr_disable();   

        //현재의 프로세스에서 I/O가 수행헤야할 시간 
        if (cur_process->time_used >= 140 && passed ==0){
            passed =1;
            //I/O를 수행한다. 
            proc_sleep(60);
        }
        //총 수행시간보다 길어지면 중지한다. 
        if (cur_process->time_used >= 200) {
            break;
        }
    
        intr_enable();
    }
}

void kernel2_proc(void* aux)
{
	int passed = 0;
	while(1)
	{
        //intr_disable , enable 로 출력에 의한 오차를 줄인다.        
        intr_disable();
        //현재의 프로세스에서 I/O가 수행헤야할 시간
        if (cur_process->time_used >= 100 && passed ==0) {
            passed=1;

            //I/O를 수행한다. 
            proc_sleep(60);
        }
        //총 수행시간보다 길어지면 중지한다. 
        if (cur_process->time_used >=120){
            break;
        }
        intr_enable();
	}
}

void kernel3_proc(void* aux)
{
	int passed1 = 0, passed2 = 0;

	while(1)
	{
        //intr_disable , enable 로 출력에 의한 오차를 줄인다.        
        intr_disable();
        //현재의 프로세스에서 I/O가 수행헤야할 시간
        if (cur_process->time_used >= 50  && passed1 ==0) {
            passed1=1;
            //I/O를 수행한다. 
            proc_sleep(60);
        }
        //현재의 프로세스에서 I/O가 수행헤야할 시간
        if (cur_process->time_used >= 100  && passed2 ==0) {
            passed2=1;
            //I/O를 수행한다. 
            proc_sleep(60);
        }
        //총 수행시간보다 길어지면 중지한다. 
        if (cur_process->time_used >=150){
            break;
        }
        intr_enable();
	}
}

/*
Let's say RQ_NQS is 5 and RQ_PPQ is 4, then the location of process with priority 7 is : 
7/4=1
0 [ ]
1 [*]
2 [ ]
3 [ ]
4 [ ]
*/

// idle process, pid = 0
void idle(void* aux)
{
    struct proc_option opt1 = { .priority = 50 };
	struct proc_option opt2 = { .priority = 50 };
//	struct proc_option opt3 = { .priority = 30 };

	proc_create(kernel1_proc, &opt1, NULL);
	proc_create(kernel2_proc, &opt2, NULL);
//	proc_create(kernel3_proc, &opt3, NULL);

    while(1) {

        schedule();  
    }
}

void proc_getpos(int priority, int *idx) 
{
	*idx = priority /RQ_PPQ ;
}

void proc_print_data()
{
	int a, b, c, d, bp, si, di, sp;

	//eax ebx ecx edx
	__asm__ __volatile("mov %%eax ,%0": "=m"(a));

	__asm__ __volatile("mov %ebx ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(b));
	
	__asm__ __volatile("mov %ecx ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(c));
	
	__asm__ __volatile("mov %edx ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(d));
	
	//ebp esi edi esp
	__asm__ __volatile("mov %ebp ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(bp));

	__asm__ __volatile("mov %esi ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(si));

	__asm__ __volatile("mov %edi ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(di));

	__asm__ __volatile("mov %esp ,%eax");
	__asm__ __volatile("mov %%eax ,%0": "=m"(sp));

	printk(	"\neax %o ebx %o ecx %o edx %o"\
			"\nebp %o esi %o edi %o esp %o\n"\
			, a, b, c, d, bp, si, di, sp);
}

void hexDump (void *addr, int len) {
    int i;
    unsigned char buff[17];
    unsigned char *pc = (unsigned char*)addr;

    if (len == 0) {
        printk("  ZERO LENGTH\n");
        return;
    }
    if (len < 0) {
        printk("  NEGATIVE LENGTH: %i\n",len);
        return;
    }

    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            if (i != 0)
                printk ("  %s\n", buff);

            printk ("  %04x ", i);
        }

        printk (" %02x", pc[i]);

        if ((pc[i] < 0x20) || (pc[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    while ((i % 16) != 0) {
        printk ("   ");
        i++;
    }

    printk ("  %s\n", buff);
}


