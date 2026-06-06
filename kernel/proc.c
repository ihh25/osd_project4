#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

// project 3: mmap_area 배열 참조
extern struct mmap_area mmap_areas[MAXMMAP];

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

const int weight_arr[40] = {
    /* -20 */ 88761,
    71755,
    56483,
    46273,
    36291,
    /* -15 */ 29154,
    23254,
    18705,
    14949,
    11916,
    /* -10 */ 9548,
    7620,
    6100,
    4904,
    3906,
    /*  -5 */ 3121,
    2501,
    1991,
    1586,
    1277,
    /*   0 */ 1024,
    820,
    655,
    526,
    423,
    /*   5 */ 335,
    272,
    215,
    172,
    137,
    /*  10 */ 110,
    87,
    70,
    56,
    45,
    /*  15 */ 36,
    29,
    23,
    18,
    15,
};
// source : https://medium.com/@vikas.singh_67409/deep-dive-into-thread-priority-in-java-be1a5da30a34

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // PA4: kalloc이 swap I/O로 sleep 가능하므로 락 일시 해제.
  // p->state = USED 상태라 다른 allocproc이 이 슬롯을 가져가지 않음.
  // AI was used (Claude) to release/reacquire p->lock around kalloc
  release(&p->lock);

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    acquire(&p->lock);
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    acquire(&p->lock);
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // PA4: 락 재취득
  acquire(&p->lock);

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->nice = 20;
  p->runtime = 0;
  p->vruntime = 0;
  p->vdeadline = 0;
  p->timeslice = 5;
  p->is_eligible = 1;
  p->last_update_time = r_time();
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;

  // PA4: 이 프로세스가 소유한 mmap_area 슬롯 정리
  // AI was used (Claude) to clean up mmap_areas on process exit
  for (int i = 0; i < MAXMMAP; i++) {
    if (mmap_areas[i].p == p) {
      mmap_areas[i].f = 0;
      mmap_areas[i].addr = 0;
      mmap_areas[i].length = 0;
      mmap_areas[i].offset = 0;
      mmap_areas[i].prot = 0;
      mmap_areas[i].flags = 0;
      mmap_areas[i].p = 0;
    }
  }

  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if (sz + n > TRAPFRAME)
    {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // PA4: uvmcopy/mmap 복사 시 swap I/O가 sleep을 유발할 수 있으므로
  // np->lock을 일시 해제. np는 아직 외부에 노출되지 않아 안전.
  // AI was used (Claude) to release/reacquire np->lock around copy paths
  release(&np->lock);

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    acquire(&np->lock);
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  // project 3: 부모의 mmap_area를 자식에게 복사
  // AI was used to copy parent's mmap_areas to the child during fork(),
  // duplicating allocated physical pages while preserving lazy state.
  for (int i = 0; i < MAXMMAP; i++)
  {
    if (mmap_areas[i].p != p)
      continue;

    // 빈 슬롯 찾기
    int slot = -1;
    for (int j = 0; j < MAXMMAP; j++)
    {
      if (mmap_areas[j].p == 0)
      {
        slot = j;
        break;
      }
    }
    if (slot == -1)
      continue;

    // mmap_area 정보 복사
    mmap_areas[slot].f = mmap_areas[i].f;
    mmap_areas[slot].addr = mmap_areas[i].addr;
    mmap_areas[slot].length = mmap_areas[i].length;
    mmap_areas[slot].offset = mmap_areas[i].offset;
    mmap_areas[slot].prot = mmap_areas[i].prot;
    mmap_areas[slot].flags = mmap_areas[i].flags;
    mmap_areas[slot].p = np; // 자식 프로세스로 설정

    // 부모에서 이미 할당된 페이지들을 자식에게도 복사
    uint64 start = mmap_areas[i].addr;
    uint64 end = start + mmap_areas[i].length;

    int perm = PTE_U;
    if (mmap_areas[i].prot & PROT_READ)
      perm |= PTE_R;
    if (mmap_areas[i].prot & PROT_WRITE)
      perm |= PTE_W;

    for (uint64 va = start; va < end; va += PGSIZE)
    {
      pte_t *pte = walk(p->pagetable, va, 0);
      if (pte == 0 || !(*pte & PTE_V))
        continue; // 아직 할당 안 된 페이지는 건너뜀

      // 새 물리 페이지 할당 후 내용 복사
      char *mem = kalloc();
      if (mem == 0)
        continue;

      uint64 pa = PTE2PA(*pte);
      memmove(mem, (char *)pa, PGSIZE);

      if (mappages(np->pagetable, va, PGSIZE, (uint64)mem, perm) != 0)
      {
        kfree(mem);
        continue;
      }
    }
  }

  // PA4: 락 재취득 (allocproc이 잡았던 락을 fork 본문에서 일시 해제했음)
  acquire(&np->lock);

  pid = np->pid;
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  // EEVDF: 부모로부터 상속
  np->nice = p->nice;
  np->vruntime = p->vruntime;
  np->runtime = 0;
  np->timeslice = 5;
  np->last_update_time = r_time();
  update_vdeadline(np); // vdeadline 재계산
  np->is_eligible = 1;

  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;

  for (;;)
  {
    intr_on();
    intr_off();

    // Step 1: v0 (최솟값 vruntime) 계산
    uint64 v0 = (uint64)-1;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        if (p->vruntime < v0)
          v0 = p->vruntime;
      }
      release(&p->lock);
    }

    // Step 2: sum_weight, sum_vw 계산
    uint64 sum_weight = 0;
    uint64 sum_vw = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        sum_weight += weight_arr[p->nice];
        sum_vw += (p->vruntime - v0) * weight_arr[p->nice];
      }
      release(&p->lock);
    }

    // Step 3: eligible 중 vdeadline 가장 작은 프로세스 선택
    struct proc *selected = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // 정수 판별식: sum_vw >= (vi - v0) * sum_weight → eligible
        uint64 rhs = (p->vruntime - v0) * sum_weight;
        p->is_eligible = (sum_vw >= rhs) ? 1 : 0;

        if (p->is_eligible)
        {
          if (selected == 0 || p->vdeadline < selected->vdeadline)
          {
            if (selected != 0)
              release(&selected->lock);
            selected = p;
            continue; // selected lock 유지
          }
        }
      }
      release(&p->lock);
    }

    // Step 4: eligible 없으면 vdeadline 가장 작은 것 선택 (fallback)
    if (selected == 0)
    {
      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          if (selected == 0 || p->vdeadline < selected->vdeadline)
          {
            if (selected != 0)
              release(&selected->lock);
            selected = p;
            continue;
          }
        }
        release(&p->lock);
      }
    }

    // Step 5: 선택된 프로세스 실행
    if (selected != 0)
    {
      selected->state = RUNNING;
      c->proc = selected;
      swtch(&c->context, &selected->context);
      c->proc = 0;
      release(&selected->lock);
    }
    else
    {
      asm volatile("wfi");
    }
    // AI 사용 (Claude)
    // 질문 : EEVDF 스케줄러를 어떻게 구현해야 하는가?
    // 조언 : 1) v0, 전체 weight합, 가중합 계산
    // 2) eligibility 판단
    // 3) eligible 중 vdeadline 가장 작은 것 선택
    // 4) 없으면 전체 중 vdeadline 가장 작은 것 선택
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1){
    panic("sched locks");
  }
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1)
    {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        // EEVDF: wakeup 시 처리
        p->timeslice = 5;
        p->last_update_time = r_time();
        update_vdeadline(p); // vdeadline 재계산
        p->is_eligible = 1;
        // sched() 호출 금지!
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// 실제 system call을 처리하는 함수들
int getnice(int pid)
{ // task 1
  struct proc *p;
  // 특정 pid를 가진 struct proc을 찾기 위해서 proc[](struct proc들의 배열)을 순회하면서 하나씩 내가 원하는 pid를 가지고 있는지 확인
  // 이 과정에서 확인하고 있는 struct proc이 변경되지 않도록 lock
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      int ans = p->nice;
      release(&p->lock);
      return ans;
    }
    release(&p->lock);
  }
  return -1;
}

int setnice(int pid, int value)
{

  if (value < 0 || value > 39)
  {
    return -1;
  }

  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->nice = value;
      release(&p->lock);
      return 0;
    }
    update_vdeadline(p);
    release(&p->lock);
  }
  return -1;
}

void ps(int pid)
{
  // procdump()를 기반으로 수정하여 작성
  static char *states[] = {
      [UNUSED] "UNUSED",
      [USED] "USED",
      [SLEEPING] "SLEEPING",
      [RUNNABLE] "RUNNABLE",
      [RUNNING] "RUNNING",
      [ZOMBIE] "ZOMBIE"};
  struct proc *p;
  char *state;

  printf("\n");

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      release(&p->lock);
      continue; // UNUSED 상태의 process는 pid가 할당되어있지 않으므로 pid를 체크하지 않고 다음 루프로 넘어가도 됨
    }

    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
    {
      state = states[p->state]; // 정상적인 범주의 state에 해당하는 숫자를 가지고 있는지 확인
    }
    else
    {
      state = "???";
    }

    uint64 rw = p->runtime * 1000 / weight_arr[p->nice];

    uint xticks;

    acquire(&tickslock); // 락을 먼저 얻고
    xticks = ticks;      // 값을 복사한 뒤
    release(&tickslock); // 락을 풉니다.

    if (pid == 0)
    {

      printf("%s %d %s %d %lu %lu %lu %lu %d %u\n",
             p->name, p->pid, state, p->nice,
             rw,
             p->runtime * 1000,
             p->vruntime,
             p->vdeadline,
             p->is_eligible,
             xticks);
      release(&p->lock);
      continue;
    }
    else if (p->pid == pid)
    {

      printf("%s %d %s %d %lu %lu %lu %lu %d %u\n",
             p->name, p->pid, state, p->nice,
             rw,
             p->runtime * 1000,
             p->vruntime,
             p->vdeadline,
             p->is_eligible,
             xticks);
      release(&p->lock);
      return;
    }
    // AI 사용 (Claude)
    // 질문 : ps()에서 EEVDF 필드를 어떻게 출력해야 하는가?
    // 조언 : tick값은 *1000 해서 millitick 단위로 출력
    release(&p->lock);
  }

  // AI(Gemini) was used
  /*
  questinon entered :
  if pid is 0, all processes should be printed. so, when pid is 0, iterate over proc[] and print all processes whose state is not UNUSED.
  if pid is not 0, iterate over proc[] and print the process when a matching pid is found. will this approach work?

  advise received :
  if implemented that way, the code would iterate over proc[] twice. therefore, it is more efficient
  to perform a single iteration and decide whether to print based on the pid value.
  */
}

int waitpid(int pid)
{
  struct proc *p;
  struct proc *myp = myproc();
  int found;

  acquire(&wait_lock);

  for (;;)
  {
    found = 0;

    // Search for the target child.
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p == myp)
        continue;
      acquire(&p->lock);
      if (p->pid == pid && p->parent == myp)
      {
        found = 1;
        if (p->state == ZOMBIE)
        {
          // Child has already exited ??clean up and return.
          int cpid = p->pid;
          (void)cpid;
          freeproc(p);
          release(&p->lock);
          release(&wait_lock);
          return 0;
        }
        release(&p->lock);
        break;
      }
      release(&p->lock);
    }

    if (!found)
    {
      // pid doesn't exist or caller is not the parent.
      release(&wait_lock);
      return -1;
    }

    // Sleep until a child exits (same channel as wait()).
    sleep(myp, &wait_lock);
  }
}
// AI(claude) was used : modifying the kwait() system call to make waitpid() system call

// project2
void update_vruntime(struct proc *p)
{
  uint64 now = r_time();
  uint64 druntime = now - p->last_update_time;
  uint64 druntime_militick = druntime / 100; // originally : 100000 = 1tick. so divide by 100

  p->vruntime += druntime_militick * 1024 / weight_arr[p->nice];
}

void update_vdeadline(struct proc *p)
{
  uint64 base_time_militick = 5 * 1000;
  p->vdeadline = p->vruntime + base_time_militick * 1024 / weight_arr[p->nice];
}