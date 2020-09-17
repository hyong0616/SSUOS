#include <list.h>
#include <proc/sched.h>
#include <mem/malloc.h>
#include <proc/proc.h>
#include <proc/switch.h>
#include <interrupt.h>

extern struct list plist;
extern struct list rlist;
extern struct list runq[RQ_NQS];

extern struct process procs[PROC_NUM_MAX];
extern struct process *idle_process;
struct process *latest;

bool more_prio(const struct list_elem *a, const struct list_elem *b,void *aux);
int scheduling; 					// interrupt.c

struct process* get_next_proc(void) 
{
    bool found = false;
	struct process *next = NULL;
	struct list_elem *elem;

    bool before = false;
    
    //0번 프로세스인 경우
    if (cur_process->pid == 0 ) {   

        //runq에서 우선순위에 따라 다음 프로세스를 리턴하기 위해 검색한다. 
        for (int i=0; i<RQ_NQS && found == false; i++) 
        {
            if (list_empty(&runq[i])) 
                continue;
        
            for(elem = list_begin(&runq[i]); elem != list_end(&runq[i]); elem = list_next(elem))
    	    {
		        struct process *p = list_entry(elem, struct process, elem_stat);

		        if(p->state == PROC_RUN && p->pid != 0) 
                {
                    found = true;
    			    next = p;
                    break;
                }
	        }
        }
    }
    //0번 프로세스가 아닌 경우 next = idle프로세스로 
    else {
       next= idle_process;
    }
    
    //현재 수행중인 프로세스가 없는 경우 next = idle  프로세스로 
    if (found == false )
        next = idle_process;
   
    return next;
}

void schedule(void)
{
    struct process *cur;
	struct process *next;

	proc_wake();

    //스케줄링하는 동안 다른 인터트의 발생시 인터럽트 핸들러가 수행하지 않게 한다. 
    intr_disable();

    bool before = false; 
    cur = cur_process;
  
    //현재의 프로세스가 0인 경우 출력한다. 
    if (cur -> pid == 0) {
    
        for (int i=1; i<PROC_NUM_MAX; i++) 
        {
            struct process *p = &procs[i];
            
            if (p->state == PROC_RUN) {
                if (!before) // 출력할 프로세스가 첫번째라면 "," 를 출력하지 않는다. 
                    printk("#= %d p= %2d c =%3d u =%3d  ",p->pid,p->priority,p->time_slice, p->time_used);
                else 
                    printk(",#= %d p= %2d c =%3d u =%3d ",p->pid,p->priority,p->time_slice, p->time_used);
                before = true;
            }
        }
    }
    if (cur->state == PROC_STOP)
        printk("Proc %d I/O at %d\n",cur->pid,cur->time_used);
	next = get_next_proc();
    //선택한 프로세스가 0이 아닌 경우 다음 프로세스의 pid값을 출력한다. 
    if (next->pid != 0)
        printk("\nSelected # = %d\n",next->pid);
    cur_process = next;
    cur_process->time_slice =0;
	
    switch_process(cur, next);
    intr_enable();
}
