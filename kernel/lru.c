// PA4 (Project 4) - LRU list of swappable user pages (SKELETON).
//
// Every physical frame currently mapped into a user page table is linked into
// a single global circular doubly-linked list ordered from least-recently-
// inserted (lru.head) to most-recently-inserted (the node just before
// lru.head).  The "clock" page-replacement algorithm walks the list from
// lru.head: if the owning PTE has PTE_A == 1, it clears the bit and moves the
// node to the tail (giving the page a second chance); otherwise the page is
// chosen as the victim.
//
// The list nodes live in the pages[] array, indexed by frame number.
// A node with pagetable == 0 means "not on the list".

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// One struct page per physical frame in [KERNBASE, PHYSTOP).
//
// Implementation note: the textbook spec says "pages[PHYSTOP / PGSIZE]", but
// in xv6-riscv that would waste ~17 MiB of BSS because PHYSTOP/PGSIZE
// includes every byte from address 0, including the large region below
// KERNBASE that is never RAM.  We size by the actual RAM range instead.
#define NPHYS_PAGES ((PHYSTOP - KERNBASE) / PGSIZE)
struct page pages[NPHYS_PAGES];

struct {
  struct spinlock lock;
  struct page    *head;   // oldest first; victim search starts here
  int             count;
} lru;

// Convert a physical address to its slot in the pages[] array.
static inline struct page *
pa_to_page(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP)
    panic("pa_to_page: bad pa");
  return &pages[(pa - KERNBASE) / PGSIZE];
}

void
lruinit(void)
{
  initlock(&lru.lock, "lru");
  lru.head  = 0;
  lru.count = 0;
  for(int i = 0; i < NPHYS_PAGES; i++){
    pages[i].next      = 0;
    pages[i].prev      = 0;
    pages[i].pagetable = 0;
    pages[i].vaddr     = 0;
  }
}

// Current length of the LRU list (for diagnostics).
int
lru_size(void)
{
  int n;
  acquire(&lru.lock);
  n = lru.count;
  release(&lru.lock);
  return n;
}

// ============================================================================
// TODO: students implement everything below this line.
// ============================================================================

//
// lru_add:
//   Attach a freshly-mapped user frame to the LRU list.  Called from
//   mappages() whenever a PTE with PTE_U is installed, and from swap_in()
//   when a frame is brought back from disk.
//
// Steps (suggested):
//   1. Look up the struct page for this pa (pa_to_page).
//   2. Acquire lru.lock.
//   3. If the node is already on the list (pg->pagetable != 0), unlink it
//      first -- we'll re-insert at the tail to reflect freshness.  This
//      keeps the list well-formed when swap_in re-adds a page that
//      somehow lingered.
//   4. Set pg->pagetable / pg->vaddr to identify the owning mapping.
//   5. Insert pg at the tail of the circular list.  Tip: the tail is
//      lru.head->prev for a non-empty list.  Be sure to handle the empty
//      list (lru.head == 0) as a special case where pg->next = pg->prev = pg.
//   6. Increment lru.count.
//   7. Release lru.lock.
//
void
lru_add(pagetable_t pt, uint64 va, uint64 pa)
{
  // TODO: implement.
  struct page* pg = pa_to_page(pa);
  acquire(&lru.lock);

  if(pg->pagetable != 0){ // if the node is already on the list    

    if (lru.count > 1) { // count가 1이면 넣었다 빼는게 의미가 없으니까 그냥 둠

      if (lru.head == pg) { // head 가리키고 있으면 다음으로 넘겨줌
        lru.head = pg->next;
      }
      // 일단 먼저 연결을 끊는다
      pg->prev->next = pg->next;
      pg->next->prev = pg->prev;

      //그리고 다시 tail에 삽입한다
      pg->prev = lru.head->prev ;
      pg->next = lru.head ;
      pg->prev->next = pg;
      lru.head->prev = pg;

      // AI used : Gemini
      // It advised me that, in cases like fork(), it is better to insert new values ​​even if they were already in the list,
      // because there can be instances where the physical address is the same but the virtual address and page table are different.
      pg->pagetable = pt;
      pg->vaddr = va;
    }
  }
  else{ // 만약 원래 없던 노드라면

    // 일단 값을 채운다
    pg->pagetable = pt;
    pg->vaddr = va;

    // 그리고 리스트에 삽입한다
    if(lru.head != 0){ // 일반적인 상황 : 노드가 한개라도 있음 -> head가 0이 아님
      // tail에 삽입한다
      pg->prev = lru.head->prev ;
      pg->next = lru.head ;
      pg->prev->next = pg;
      lru.head->prev = pg;

    }else{ // 안 일반적인 상황 : 노드가 한개도 없다.
      pg->next = pg->prev = pg;
      lru.head = pg ;
    }

    lru.count++ ;
  }
    release(&lru.lock);

    return ;
}

//
// lru_remove:
//   Detach a frame from the LRU list.  Called from uvmunmap() whenever a
//   valid user PTE is torn down, so the bookkeeping matches the actual
//   user mappings.
//
// Steps (suggested):
//   1. Look up the struct page (pa_to_page).
//   2. Acquire lru.lock.
//   3. If pg->pagetable == 0, the page wasn't on the list (e.g. swapped
//      out, or never tracked) -- just release the lock and return.
//   4. Unlink pg from the circular list:
//        - If pg is the only element, set lru.head = 0.
//        - Otherwise patch pg->prev and pg->next to skip pg, and if pg
//          was the head, advance lru.head.
//   5. Clear pg->pagetable / pg->vaddr / pg->next / pg->prev.
//   6. Decrement lru.count.
//   7. Release lru.lock.
//
void
lru_remove(uint64 pa)
{
  // TODO: implement.
  struct page* pg = pa_to_page(pa);
  acquire(&lru.lock);

  if(pg->pagetable == 0){ // 애초에 리스트에 없음
    release(&lru.lock);
    return ;
  }
  else{ // 그게 아니면

    if(lru.count == 1){ // pg가 the only element였음
      lru.head = 0;
    }
    else{
      // AI used : Geminin
      // Question : Should this unlink be performed even on a list with only one node?
      // Answer : When there is only one node, there is no need to touch it at all because pg->prev = pg->next = pg. 
      // It is better to put it inside the else conditional statement.
      // pg를 circular list에서 unlink
      pg->prev->next = pg->next;
      pg->next->prev = pg->prev;  

      if(lru.head == pg){ // pg가 head였으면
        lru.head = pg->next ; // 한칸 옮겨
      }
    }

    // 그리고 pg를 지워
    pg->next = 0;
    pg->prev = 0;
    pg->pagetable = 0; 
    pg->vaddr = 0;

    lru.count-- ;

    release(&lru.lock);
    return;
  }
}

//
// lru_select_victim:
//   The clock-algorithm core.  Walk the list from lru.head; for each
//   candidate, look at the owning PTE's PTE_A bit:
//
//     PTE_A == 1 : the page was accessed recently.  Clear PTE_A (so it
//                  becomes a candidate next time), rotate the node to
//                  the tail (advance lru.head), and continue.
//     PTE_A == 0 : pick this page as the victim.  Unlink it from the LRU
//                  and return its physical address.  Write the owning
//                  pagetable and vaddr into *out_pt / *out_va so the
//                  caller (swap_out) can find the right PTE.
//
//   Returns 0 if the LRU is empty.
//
// Termination guarantee:
//   After one full pass clears every PTE_A bit, the next pass is
//   guaranteed to find a page with PTE_A == 0.  Bound the loop at
//   2 * lru.count iterations as a safety net.
//
// Implementation notes:
//   * Use walk(pg->pagetable, pg->vaddr, 0) to obtain the PTE pointer.
//   * After clearing PTE_A, sfence_vma() so the change is visible to the
//     MMU on subsequent accesses.
//   * Defensive check: if walk() returns 0 or the PTE has PTE_V == 0,
//     the LRU node is stale; drop it (unlink + clear pagetable/vaddr)
//     and continue with the next candidate.
//   * Don't forget to release lru.lock on every exit path.
//
// Useful symbols:
//   PTE_A, PTE_V             -- riscv.h
//   PTE2PA(pte)              -- extract the physical address from a PTE
//   walk(pt, va, alloc=0)    -- page-table walk
//   sfence_vma()             -- TLB flush
//
// AI was used (Claude) to assist with this implementation
uint64
lru_select_victim(pagetable_t *out_pt, uint64 *out_va)
{
  acquire(&lru.lock);

  if(lru.head == 0){
    release(&lru.lock);
    return 0;  // LRU 비어있음
  }

  // 안전망: 최대 2 * count 회만 순회 (한 바퀴는 PTE_A 클리어, 다음 바퀴에 victim 발견 보장)
  int max_iter = 2 * lru.count + 2;

  for(int i = 0; i < max_iter; i++){
    if(lru.head == 0){
      release(&lru.lock);
      return 0;
    }

    struct page *pg = lru.head;
    pte_t *pte = walk(pg->pagetable, pg->vaddr, 0);

    // Stale 노드 방어: PTE 없거나 무효면 LRU에서 제거하고 다음으로
    if(pte == 0 || (*pte & PTE_V) == 0){
      // unlink pg
      if(pg->next == pg){
        lru.head = 0;
      } else {
        pg->prev->next = pg->next;
        pg->next->prev = pg->prev;
        lru.head = pg->next;
      }
      pg->next = pg->prev = 0;
      pg->pagetable = 0;
      pg->vaddr = 0;
      lru.count--;
      continue;
    }

    if(*pte & PTE_A){
      // 최근 사용됨 → second chance: PTE_A 클리어, 노드를 꼬리로 (head 전진)
      *pte &= ~PTE_A;
      sfence_vma();
      lru.head = pg->next;  // head를 다음으로 진행 (= pg를 꼬리로 보낸 효과)
      continue;
    }

    // PTE_A == 0 → 이 페이지를 victim으로 선택
    uint64 pa = PTE2PA(*pte);
    *out_pt = pg->pagetable;
    *out_va = pg->vaddr;

    // LRU에서 unlink
    if(pg->next == pg){
      lru.head = 0;
    } else {
      pg->prev->next = pg->next;
      pg->next->prev = pg->prev;
      if(lru.head == pg)
        lru.head = pg->next;
    }
    pg->next = pg->prev = 0;
    pg->pagetable = 0;
    pg->vaddr = 0;
    lru.count--;

    release(&lru.lock);
    return pa;
  }

  // 안전망 초과 (이론상 도달 불가)
  release(&lru.lock);
  return 0;
}
