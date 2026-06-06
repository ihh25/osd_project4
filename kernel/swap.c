// PA4 (Project 4) - Page Replacement: swap.c (SKELETON)
//
// This file ships with helpers already implemented for you:
//   * swapread / swapwrite      : page-granular disk I/O against the swap
//                                  area at the tail of fs.img
//                                  (disk blocks [SWAPBASE, SWAPBASE+SWAPMAX)).
//   * swapstat                   : exposes how many disk blocks have been
//                                  read/written for swap I/O since boot.
//   * swap_alloc_slot /
//     swap_free_slot             : reserve and release page-sized slots in
//                                  the swap-slot bitmap.
//   * swapinit                   : called once at boot to set everything up.
//
// You will implement:
//   * swap_out                   : pick a victim via the clock algorithm,
//                                  write it to the swap area, rewrite the
//                                  owning PTE, return the freed frame.
//   * swap_in                    : page-fault handler entry; restore a
//                                  swapped-out page from the swap area.
//
// One swap "slot" holds exactly one PGSIZE page; each slot occupies
// (PGSIZE / BSIZE) = 4 consecutive disk blocks.  The bitmap therefore has
// (SWAPMAX / 4) bits = 7000 bits, which fits in one page.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"   // BSIZE
#include "buf.h"  // struct buf (data field accessed directly)

#define BLKS_PER_PAGE (PGSIZE / BSIZE)       // 4
#define NSWAPSLOTS (SWAPMAX / BLKS_PER_PAGE) // total page-sized slots

// I/O statistics (disk blocks, not pages).
struct
{
  struct spinlock lock;
  int nr_sectors_read;
  int nr_sectors_write;
} swapstats;

// Swap-slot bitmap.  bitmap[i] == 1  =>  slot i is occupied.
// Allocated once at boot from a kalloc'd page.
struct
{
  struct spinlock lock;
  uchar *bits; // length: NSWAPSLOTS bits, stored in a single page
  uint next;   // hint for next allocation (round-robin)
} swapmap;

void swapinit(void)
{
  initlock(&swapstats.lock, "swapstats");
  swapstats.nr_sectors_read = 0;
  swapstats.nr_sectors_write = 0;

  initlock(&swapmap.lock, "swapmap");
  swapmap.bits = (uchar *)kalloc();
  if (swapmap.bits == 0)
    panic("swapinit: kalloc bitmap");
  memset(swapmap.bits, 0, PGSIZE);
  swapmap.next = 0;
}

// Allocate a free swap slot.  Returns -1 if the swap is full.
int swap_alloc_slot(void)
{
  acquire(&swapmap.lock);
  for (uint tries = 0; tries < NSWAPSLOTS; tries++)
  {
    uint i = (swapmap.next + tries) % NSWAPSLOTS;
    uint byte = i >> 3;
    uchar mask = 1 << (i & 7);
    if ((swapmap.bits[byte] & mask) == 0)
    {
      swapmap.bits[byte] |= mask;
      swapmap.next = (i + 1) % NSWAPSLOTS;
      release(&swapmap.lock);
      return (int)i;
    }
  }
  release(&swapmap.lock);
  return -1;
}

// Mark a swap slot as free.
void swap_free_slot(uint slot)
{
  if (slot >= NSWAPSLOTS)
    panic("swap_free_slot: bad slot");
  acquire(&swapmap.lock);
  uint byte = slot >> 3;
  uchar mask = 1 << (slot & 7);
  if ((swapmap.bits[byte] & mask) == 0)
  {
    release(&swapmap.lock);
    panic("swap_free_slot: double-free");
  }
  swapmap.bits[byte] &= ~mask;
  release(&swapmap.lock);
}

// Read one page (PGSIZE bytes) starting at physical address `ptr` from
// swap slot `blkno`.  This is the slot index, not a raw disk block number.
//
// Internally, 4 consecutive disk blocks are read into the buffer cache
// and copied out.  Statistics are bumped by BLKS_PER_PAGE.
void swapread(uint64 ptr, int blkno)
{
  if (blkno < 0 || blkno >= NSWAPSLOTS)
    panic("swapread: bad slot");

  uint disk_blk = SWAPBASE + (uint)blkno * BLKS_PER_PAGE;
  char *dst = (char *)ptr;

  for (int i = 0; i < BLKS_PER_PAGE; i++)
  {
    struct buf *b = bread(ROOTDEV, disk_blk + i);
    memmove(dst + i * BSIZE, b->data, BSIZE);
    brelse(b);
  }

  acquire(&swapstats.lock);
  swapstats.nr_sectors_read += BLKS_PER_PAGE;
  release(&swapstats.lock);
}

// Write one page (PGSIZE bytes) starting at physical address `ptr` into
// swap slot `blkno`.  Counterpart of swapread.
void swapwrite(uint64 ptr, int blkno)
{
  if (blkno < 0 || blkno >= NSWAPSLOTS)
    panic("swapwrite: bad slot");

  uint disk_blk = SWAPBASE + (uint)blkno * BLKS_PER_PAGE;
  char *src = (char *)ptr;

  for (int i = 0; i < BLKS_PER_PAGE; i++)
  {
    struct buf *b = bread(ROOTDEV, disk_blk + i);
    memmove(b->data, src + i * BSIZE, BSIZE);
    bwrite(b);
    brelse(b);
  }

  acquire(&swapstats.lock);
  swapstats.nr_sectors_write += BLKS_PER_PAGE;
  release(&swapstats.lock);
}

// Expose the running counters to user space.  Both arguments are
// kernel-side pointers; the caller (sys_swapstat) does the copyout.
void swapstat(int *nr_sectors_read, int *nr_sectors_write)
{
  acquire(&swapstats.lock);
  if (nr_sectors_read)
    *nr_sectors_read = swapstats.nr_sectors_read;
  if (nr_sectors_write)
    *nr_sectors_write = swapstats.nr_sectors_write;
  release(&swapstats.lock);
}

// ============================================================================
// TODO: students implement everything below this line.
// ============================================================================

//
// swap_out:
//   Free a physical frame by evicting one of the user-mapped pages.
//
// Steps (suggested):
//   1. Ask the LRU subsystem for a victim:
//        uint64 pa = lru_select_victim(&pt, &va);
//      lru_select_victim returns the page's physical address and unlinks
//      it from the LRU list.  pt and va identify the owning PTE.
//      If the LRU is empty it returns 0.
//
//   2. Reserve a free swap slot with swap_alloc_slot().  If that fails
//      (-1), the swap area is full -- you must restore the victim to the
//      LRU (lru_add) and signal failure to the caller (return 0).
//
//   3. Write the victim's contents to that slot with swapwrite(pa, slot).
//
//   4. Walk the owning page table to find the victim's PTE and rewrite it:
//        - Replace the PPN with the swap slot index (use SLOT2PTE).
//        - Clear PTE_V (page is no longer resident).
//        - Set PTE_S (page is swapped out -- the page-fault handler will
//          recognize this).
//        - Preserve PTE_U / PTE_R / PTE_W / PTE_X.
//
//   5. Return the freed physical frame (pa cast to void *) so that the
//      caller (kalloc) can hand it to the next allocation request.
//
// On any failure, return 0.  The caller in kalloc.c will treat that as
// "no memory available" and propagate the failure upward.
//
// Useful symbols from defs.h / riscv.h:
//   walk(pt, va, alloc=0)  -- page-table walk; returns pte_t * or 0
//   PTE_FLAGS(pte)         -- extract the lower-12-bit flag field
//   SLOT2PTE(slot)         -- encode a slot index into a PTE's PPN
//   PTE_V, PTE_S, PTE_A, PTE_U, PTE_R, PTE_W, PTE_X
//   lru_select_victim(...) -- declared in defs.h
//   lru_add(pt, va, pa)    -- re-insert a frame into the LRU (used on
//                             failure to put the page back where it was)
//
// AI was used (Claude) to assist with this implementation
void *
swap_out(void)
{
  pagetable_t pt;
  uint64 va;
  uint64 pa;
  pte_t *pte;
  int slot;

  // 1. Victim 선택 (LRU에서 자동 unlink됨)
  pa = lru_select_victim(&pt, &va);
  if (pa == 0)
    return 0; // LRU 비어있음

  // 2. 빈 swap slot 확보
  slot = swap_alloc_slot();
  if (slot < 0)
  {
    // swap 영역 꽉 참 → victim을 LRU에 복귀시키고 실패
    lru_add(pt, va, pa);
    return 0;
  }

  // 3. Victim 페이지 내용을 디스크에 기록
  swapwrite(pa, slot);

  // 4. Victim의 PTE 갱신
  pte = walk(pt, va, 0);
  if (pte == 0)
  {
    // 방어: walk 실패 시 슬롯 회수하고 실패
    swap_free_slot(slot);
    lru_add(pt, va, pa);
    return 0;
  }

  // 기존 perm 비트 (PTE_U/R/W/X) 유지하면서 PPN을 slot으로, V→0, S→1
  uint64 flags = PTE_FLAGS(*pte) & (PTE_U | PTE_R | PTE_W | PTE_X);
  *pte = SLOT2PTE(slot) | flags | PTE_S;
  // PTE_V는 위에서 마스킹해서 빠짐, PTE_S는 OR로 set

  sfence_vma();

  // 5. 비워진 프레임을 호출자에게 반환 (kalloc이 새 용도로 사용)
  return (void *)pa;
}

//
// swap_in:
//   Bring a swapped-out page back into memory.  Called from the page-fault
//   handler when usertrap() sees a fault on a PTE that has PTE_V == 0 and
//   PTE_S == 1.
//
// Arguments:
//   pt -- the faulting process's page table
//   va -- the virtual address that triggered the fault
//
// Steps (suggested):
//   1. va = PGROUNDDOWN(va) -- always operate on the page boundary.
//
//   2. Walk the page table to get the PTE.  Sanity-check that it really
//      is a swapped-out entry (PTE_V == 0 and PTE_S == 1).  If not, this
//      isn't a page we can swap in -- return -1 and let the caller treat
//      the fault as a real segfault.
//
//   3. Extract the swap slot index with PTE2SLOT(*pte).
//
//   4. Allocate a fresh physical frame with kalloc().  Note: this call
//      may *itself* recursively trigger swap_out (that's fine, by design)
//      but it can still return 0 if both RAM and swap are exhausted.
//      Return -1 in that case.
//
//   5. Read the slot's contents into the new frame with swapread(...).
//      Mark the slot as free with swap_free_slot(...).
//
//   6. Rewrite the PTE to point at the new frame:
//        - PPN field <- new physical address (use PA2PTE).
//        - Set PTE_V, clear PTE_S.
//        - Preserve PTE_U / PTE_R / PTE_W / PTE_X.
//
//   7. Re-attach the new frame to the LRU list with lru_add(...).
//
//   8. Call sfence_vma() to flush the stale TLB entry.  The faulting
//      instruction will then re-execute and succeed.
//
//   Return 0 on success.
//
// AI was used (Claude) to assist with this implementation
int swap_in(pagetable_t pt, uint64 va)
{
  pte_t *pte;
  uint slot;
  char *mem;

  // 1. 페이지 경계로 정렬
  va = PGROUNDDOWN(va);

  // 2. PTE 검증 — 정말 swap된 페이지인지
  pte = walk(pt, va, 0);
  if (pte == 0)
    return -1;
  if ((*pte & PTE_V) || !(*pte & PTE_S))
    return -1; // swap된 페이지 아님 → 진짜 segfault

  // 3. Slot 번호 추출
  slot = PTE2SLOT(*pte);

  // 4. 새 물리 프레임 할당
  // (이 kalloc이 swap_out을 재귀적으로 부를 수 있음 — 정상)
  mem = kalloc();
  if (mem == 0)
    return -1; // RAM+swap 둘 다 꽉 참

  // 5. 디스크에서 슬롯 내용 복사 + 슬롯 해제
  swapread((uint64)mem, slot);
  swap_free_slot(slot);

  // 6. PTE 갱신: 새 프레임 주소 + PTE_V=1, PTE_S=0
  uint64 flags = PTE_FLAGS(*pte) & (PTE_U | PTE_R | PTE_W | PTE_X);
  *pte = PA2PTE((uint64)mem) | flags | PTE_V;
  // PTE_S 빠짐 (위 마스킹에서 빠짐)

  // 7. LRU에 추가 (재귀 swap_out이 이걸 victim으로 안 고르도록 마지막에)
  lru_add(pt, va, (uint64)mem);

  // 8. TLB flush
  sfence_vma();

  return 0;
}
