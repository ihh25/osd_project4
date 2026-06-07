#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//wrapper 함수 : 유저 상태에서 시스템콜을 호출해서 인자를 전달받았음
// 여기서 arg어쩌고들을 통해서 인자를 커널쪽으로 넘겨주고 커널모드의 시스템콜을 호출

uint64
sys_getnice(void)
{
  int pid;
  argint(0, &pid);
  return getnice(pid);
}

// sys_setnice: int setnice(int pid, int value)
uint64
sys_setnice(void)
{
  int pid, value;
  argint(0, &pid);
  argint(1, &value);
  return setnice(pid, value);
}

// sys_ps: void ps(int pid)
uint64
sys_ps(void)
{
  int pid;
  argint(0, &pid);
  ps(pid);
  return 0;
}

// sys_meminfo: int meminfo(void)
uint64
sys_meminfo(void)
{
  return meminfo();
}

// sys_waitpid: int waitpid(int pid)
uint64
sys_waitpid(void)
{
  int pid;
  argint(0, &pid);
  return waitpid(pid);
}

// sys_freemem : int freemem(void)
uint64
sys_freemem(void)
{
  return freemem();
}

uint64
sys_swapstat(void)
{
    uint64 ra_addr, wa_addr;
    int r, w;
    argaddr(0, &ra_addr);
    argaddr(1, &wa_addr);
    swapstat(&r, &w);
    
    if(copyout(myproc()->pagetable, ra_addr, (char *)&r, sizeof(r)) < 0)
        return -1;
    if(copyout(myproc()->pagetable, wa_addr, (char *)&w, sizeof(w)) < 0)
        return -1;
    return 0;
}