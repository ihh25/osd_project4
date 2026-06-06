#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

// forward declaration
void prepare_return(void);

// project 3: mmap_area 배열 참조
extern struct mmap_area mmap_areas[MAXMMAP];

void trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64
usertrap(void)
{
  int which_dev = 0;

  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec); // DOC: kernelvec

  struct proc *p = myproc();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  if (r_scause() == 8)
  {
    // system call

    if (killed(p))
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  }
  else if (r_scause() == 12 || r_scause() == 13 || r_scause() == 15)
  {
    // 12 = instruction page fault
    // 13 = load page fault (read)
    // 15 = store page fault (write)
    // AI was used (Claude) to integrate PA4 swap-in handler with PA3 mmap handler
    uint64 fault_addr = r_stval();
    int is_write = (r_scause() == 15);
    int handled = 0;

    // PA4: swap된 페이지면 swap_in 먼저 시도
    if (fault_addr < MAXVA)
    {
      pte_t *pte_pa4 = walk(p->pagetable, fault_addr, 0);
      if (pte_pa4 && !(*pte_pa4 & PTE_V) && (*pte_pa4 & PTE_S))
      {
        if (swap_in(p->pagetable, fault_addr) == 0)
        {
          handled = 1;
        }
        else
        {
          setkilled(p);
          handled = 1;
        }
      }
    }

    // PA4 swap-in으로 처리 못 한 경우 PA3 mmap 처리
    if (!handled)
    {
      for (int i = 0; i < MAXMMAP; i++)
      {
        struct mmap_area *ma = &mmap_areas[i];
        if (ma->p != p)
          continue;
        if (fault_addr < ma->addr || fault_addr >= ma->addr + ma->length)
          continue;

        if (is_write && !(ma->prot & PROT_WRITE))
          break;

        uint64 va = PGROUNDDOWN(fault_addr);

        char *mem = kalloc();
        if (mem == 0)
          break;
        memset(mem, 0, PGSIZE);

        if (!(ma->flags & MAP_ANONYMOUS) && ma->f != 0)
        {
          int page_offset = (int)(va - ma->addr);
          ilock(ma->f->ip);
          readi(ma->f->ip, 0, (uint64)mem, ma->offset + page_offset, PGSIZE);
          iunlock(ma->f->ip);
        }

        int perm = PTE_U;
        if (ma->prot & PROT_READ)
          perm |= PTE_R;
        if (ma->prot & PROT_WRITE)
          perm |= PTE_W;

        if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0)
        {
          kfree(mem);
          break;
        }

        handled = 1;
        break;
      }

      // PA3 lazy sbrk fallback (가드: 유효 VA 범위 안)
      if (!handled && fault_addr < p->sz &&
          vmfault(p->pagetable, fault_addr, is_write ? 0 : 1) != 0)
      {
        handled = 1;
      }

      if (!handled)
      {
        setkilled(p);
      }
    }
  }
  else if ((which_dev = devintr()) != 0)
  {
    // ok
  }
  else
  {
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if (killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    if (p != 0 && p->state == RUNNING)
    {
      if (p->timeslice <= 0)
      {
        p->timeslice = 5;
        update_vdeadline(p);
        yield();
      }
    }
  }

  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
}

//
// set up trapframe and control registers for a return to user space
//
void prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  struct proc *p = myproc();

  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0)
  {
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    if (p != 0 && p->state == RUNNING)
    {
      if (p->timeslice <= 0)
      {
        p->timeslice = 5;
        update_vdeadline(p);
        yield();
      }
    }
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void clockintr()
{
  if (cpuid() == 0)
  {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }
  // EEVDF: p->lock 없이 atomic하게 카운터만 갱신
  struct proc *p = myproc();
  if (p != 0 && p->state == RUNNING)
  {
    p->runtime++;
    p->last_update_time = r_time();
    p->timeslice--;
    update_vruntime(p);
  }

  w_stimecmp(r_time() + 100000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr()
{
  uint64 scause = r_scause();

  if (scause == 0x8000000000000009L)
  {
    // this is a supervisor external interrupt, via PLIC.

    int irq = plic_claim();

    if (irq == UART0_IRQ)
    {
      uartintr();
    }
    else if (irq == VIRTIO0_IRQ)
    {
      virtio_disk_intr();
    }
    else if (irq)
    {
      printf("unexpected interrupt irq=%d\n", irq);
    }

    if (irq)
      plic_complete(irq);

    return 1;
  }
  else if (scause == 0x8000000000000005L)
  {
    // timer interrupt.
    clockintr();
    return 2;
  }
  else
  {
    return 0;
  }
}