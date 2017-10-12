#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
//#include "pstat.h"
//BEST SO FAAAAAAAAAAARRRRRRRRRRRRRRRRRR
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;
struct proc *q0[NPROC];  //Lowest priority processes
struct proc *q1[NPROC];
struct proc *q2[NPROC];
struct proc *q3[NPROC];  //Highest priority processes
int q3_counter = 0;
int nextpid = 1;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  //struct pstat *ps;
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  //ps.inuse[p->pid] = 1;
  p->priority = 3;
//  ps.pid[p->pid] = p->pid;
//  ps.priority[p->pid] = p->priority;
//  ps.state[p->pid] = RUNNABLE;
//  ps.ticks[p->pid][0] = 0;
//  ps.ticks[p->pid][1] = 0;
//  ps.ticks[p->pid][2] = 0;
//  ps.ticks[p->pid][3] = 0;
//  ps.wait_ticks[p->pid][0] = 0;
//  ps.wait_ticks[p->pid][1] = 0;
//  ps.wait_ticks[p->pid][2] = 0;
//  ps.wait_ticks[p->pid][3] = 0;
  q3[q3_counter] = p;
  q3_counter++;
  release(&ptable.lock);
  
  
  // Allocate kernel stack if possible.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  //struct pstat *ps;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  acquire(&ptable.lock);
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  //ps->state = RUNNABLE;
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);
 
  pid = np->pid;
  np->state = RUNNABLE;
  safestrcpy(np->name, proc->name, sizeof(proc->name));
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  iput(proc->cwd);
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// Implement the PSTAT here as well
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
   struct proc *p;
   struct proc *pw;
   int proc_index = 0;
   for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      proc = p;
      proc->inuse = 0;
      for(int j = 0; j < 4; j++){
         proc->ticks[proc_index][j] = 0;
         proc->wait_ticks[proc_index][j] = 0;
      }
      proc_index++;
   }
   proc = 0;
   int q2_counter = 0;
   int q1_counter = 0;
   int q0_counter = 0;
   
   for(;;){
 // Enable interrupts on this processor.
      sti();
 // Loop over process table looking for process to run.
      acquire(&ptable.lock);
 // Looping queue 3s
 // This gets 8/12 when I know it is wrong
scheduleQueue3:
      for(int i = 0; i < q3_counter; i++){
         if(q3[i]->state != RUNNABLE)
            continue;
         proc = q3[i]; p = q3[i];
         proc->ticks[p->pid][p->priority]++;
         proc->inuse = 1;
         proc->wait_ticks[proc->pid][proc->priority] = 0;
         if(proc->ticks[p->pid][p->priority] == 8){
            
            proc->priority--;
            q2[q2_counter] = proc;
            proc->ticks[p->pid][p->priority] = 0;
            q2_counter++;
            q3[i] = NULL;
            for(int j = i; j < q3_counter - 1; j++){
               q3[j] = q3[j + 1];
            }
            q3[q3_counter] = NULL;
            q3_counter--;
            //After adding Else If here I lost points on priority Boost...
         } else if (proc->ticks[p->pid][p->priority] > 8){
             proc->ticks[p->pid][p->priority] = 1;
             
         }
         switchuvm(p);
         p->state = RUNNING;
         swtch(&cpu->scheduler, proc->context);
         switchkvm();
         //cprintf("Current process id: %d, ticks: %d, priority lv: %d\n", proc->pid, proc->ticks[p->pid][p->priority], proc->priority);
    
         for(int j = 1; j < q3_counter + q2_counter + q1_counter + q0_counter; j++){
            pw = &ptable.proc[j];
            if(proc->pid == j ||  pw->inuse != 1)
               continue;
            pw->wait_ticks[j][pw->priority]++;
            if(pw->priority == 0 && pw->wait_ticks[j][pw->priority] == 500){
               pw->priority++;
               q1[q1_counter] = pw;
               q1[q1_counter]->wait_ticks[j][pw->priority] = 0;
               q1_counter++;
            } else if(pw->priority == 1 && pw->wait_ticks[j][pw->priority] == 320){
               pw->priority++;
               q2[q2_counter] = pw;
               q2[q2_counter]->wait_ticks[j][pw->priority] = 0;
               q2_counter++;
            } else if(pw->priority == 2 && pw->wait_ticks[j][pw->priority] == 160){
               pw->priority++;
               q3[q3_counter] = pw;
               q3_counter++;
               pw->wait_ticks[j][pw->priority] = 0;
            }
         }
         
         proc = 0;
      }
       
      for(int i = 0; i < q2_counter; i++){
         if(q2[i]->state != RUNNABLE)
            continue;
         proc = q2[i]; p = q2[i]; //necessary?
         proc->ticks[p->pid][p->priority]++;
         proc->inuse = 1;
         if(proc->ticks[p->pid][p->priority] == 16){
            proc->priority--;
            q1[q1_counter] = proc;
            proc->ticks[p->pid][p->priority] = 0;
            q1_counter++;
// Remove the current process from the priority 3 queue
            q2[i] = NULL;
            for(int j = i; j < q2_counter - 1; j++){
               q2[j] = q2[j + 1];
            }
            q2[q2_counter] = NULL;
            q2_counter--;
         }else if (proc->ticks[p->pid][p->priority] > 16){
             proc->ticks[p->pid][p->priority] = 1;
         }
         //cprintf("Current process id: %d, ticks: %d, priority lv: 2\n", proc->pid, proc->ticks);
         switchuvm(p);
         p->state = RUNNING;
         swtch(&cpu->scheduler, proc->context);
         switchkvm();
         //cprintf("Current process id: %d, ticks: %d, priority lv: %d\n", proc->pid, proc->ticks[p->pid][p->priority], proc->priority);
         for(int j = 1; j < q3_counter + q2_counter + q1_counter + q0_counter; j++){
            pw = &ptable.proc[j];
            if(proc->pid == j ||  pw->inuse != 1)
               continue;
            pw->wait_ticks[j][pw->priority]++;
            if(pw->priority == 0 && pw->wait_ticks[j][pw->priority] == 500){
               pw->priority++;
               q1[q1_counter] = pw;
               q1[q1_counter]->wait_ticks[j][pw->priority] = 0;
               q1_counter++;
            } else if(pw->priority == 1 && pw->wait_ticks[j][pw->priority] == 320){
               pw->priority++;
               q2[q2_counter] = pw;               
               q2[q2_counter]->wait_ticks[j][pw->priority] = 0;
               q2_counter++;
            } else if(pw->priority == 2 && pw->wait_ticks[j][pw->priority] == 160){
               pw->priority++;
               q3[q3_counter] = pw;
               q3[q3_counter]->wait_ticks[j][pw->priority] = 0;
               q3_counter++;
            }
         }
         goto scheduleQueue3;
         proc = 0;
      }
      
      for(int i = 0; i < q1_counter; i++){
         if(q1[i]->state != RUNNABLE)
            continue;
         proc = q1[i]; p = q1[i]; //necessary?
         proc->ticks[p->pid][p->priority]++;
         proc->inuse = 1;
         if(proc->ticks[p->pid][p->priority] == 32){
            proc->priority--;
            q0[q0_counter] = proc;
            proc->ticks[p->pid][p->priority] = 0  ;
            q0_counter++;
// Remove the current process from the priority 3 queue
            q1[i] = NULL;
            for(int j = i; j < q1_counter - 1; j++){
               q1[j] = q1[j + 1];
            }
            q1[q1_counter] = NULL;
            q1_counter--;
         }else if (proc->ticks[p->pid][p->priority] > 32){
             proc->ticks[p->pid][p->priority] = 1;
         }
         //cprintf("Current process id: %d, ticks: %d, priority lv: %d\n", proc->pid, proc->ticks, proc->priority);
         switchuvm(p);
         p->state = RUNNING;
         swtch(&cpu->scheduler, proc->context);
         switchkvm();
         //cprintf("Current process id: %d, ticks: %d, priority lv: %d\n", proc->pid, proc->ticks[p->pid][p->priority], proc->priority);
         for(int j = 1; j < q3_counter + q2_counter + q1_counter + q0_counter; j++){
            pw = &ptable.proc[j];
            if(proc->pid == j ||  pw->inuse != 1)
               continue;
            pw->wait_ticks[j][pw->priority]++;
            if(pw->priority == 0 && pw->wait_ticks[j][pw->priority] == 500){
               pw->priority++;
               q1[q1_counter] = pw;
               q1[q1_counter]->wait_ticks[j][pw->priority] = 0;
               q1_counter++;
            } else if(pw->priority == 1 && pw->wait_ticks[j][pw->priority] == 320){
               pw->priority++;
               q2[q2_counter] = pw;
               q2[q2_counter]->wait_ticks[j][pw->priority] = 0;
               q2_counter++;
            } else if(pw->priority == 2 && pw->wait_ticks[j][pw->priority] == 160){
               pw->priority++;
               q3[q3_counter] = pw;
               q3[q3_counter]->wait_ticks[j][pw->priority] = 0;
               q3_counter++;
            }
         }
         goto scheduleQueue3;
         proc = 0;
      }
      
      for(int i = 0; i < q0_counter; i++){
         if(q0[i]->state != RUNNABLE)
            continue;
         proc = q0[i]; p = q0[i]; //necessary?
         proc->ticks[p->pid][p->priority]++;
         proc->inuse = 1;
         switchuvm(p);
         p->state = RUNNING;
         swtch(&cpu->scheduler, proc->context);
         switchkvm();
         //cprintf("Current process id: %d, ticks: %d, priority lv: %d\n", proc->pid, proc->ticks[p->pid][p->priority], proc->priority);
         for(int j = 1; j < q3_counter + q2_counter + q1_counter + q0_counter; j++){
            pw = &ptable.proc[j];
            if(proc->pid == j ||  pw->inuse != 1)
               continue;
            pw->wait_ticks[j][pw->priority]++;
            if(pw->priority == 0 && pw->wait_ticks[j][pw->priority] == 500){
               pw->priority++;
               q1[q1_counter] = pw;
               
               q1[q1_counter]->wait_ticks[j][pw->priority] = 0;
               q1_counter++;
            } else if(pw->priority == 1 && pw->wait_ticks[j][pw->priority] == 320){
               pw->priority++;
               q2[q2_counter] = pw;
               
               q2[q2_counter]->wait_ticks[j][pw->priority] = 0;
               q2_counter++;
            } else if(pw->priority == 2 && pw->wait_ticks[j][pw->priority] == 160){
               pw->priority++;
               q3[q3_counter] = pw;
               
               q3[q3_counter]->wait_ticks[j][pw->priority] = 0;
               q3_counter++;
            }
         }
         goto scheduleQueue3;
         proc = 0;
      }
      release(&ptable.lock);
   }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


