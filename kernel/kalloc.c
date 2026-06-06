// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
} kmem;

void kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// AI was used (Claude) to integrate swap-out fallback
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r)
  {
    memset((char *)r, 5, PGSIZE); // fill with junk
    return (void *)r;
  }

  // PA4: free-list 비어있음 → swap_out으로 페이지 확보 시도
  void *p = swap_out();
  if (p)
    memset((char *)p, 5, PGSIZE);
  return p;
}

uint64
meminfo(void)
{
  struct run *r;
  uint64 count = 0;

  acquire(&kmem.lock);
  r = kmem.freelist;
  while (r)
  {
    count++;
    r = r->next;
  }
  release(&kmem.lock);

  return count * PGSIZE;
}
// AI(claude) was used : using uint64 instead of int
/*
because the maximum value of type int is 2GB, therefore when the memory exceeds 2GB, an overflow occurs and the value becomes negative.
and the meminfo() system call has no error case, so it does not return -1.
*/

// project 3: 현재 free 페이지 수 반환
// AI was used to implement freemem() by traversing kmem.freelist under lock.
int freemem(void)
{
  struct run *r;
  int count = 0;

  acquire(&kmem.lock);
  r = kmem.freelist;
  while (r)
  {
    count++;
    r = r->next;
  }
  release(&kmem.lock);

  return count;
}