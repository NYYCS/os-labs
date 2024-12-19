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

struct run {
  struct run *next;
};

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];

void
steal()
{
  struct run *r;
  struct kmem *k1, *k2;

  push_off();

  k1 = &kmems[cpuid()];

  for(k2 = kmems; k2 < &kmems[NCPU]; k2++){
    if(k1 == k2)
      continue;
    acquire(&k2->lock);
    r = k2->freelist;
    if(r){
      k2->freelist = r->next;
      r->next = k1->freelist;
      k1->freelist = r;
    }
    release(&k2->lock);
  }

  pop_off();
}

void
kinit()
{
  struct kmem *k;
  for(k = kmems; k < &kmems[NCPU]; k++)
    initlock(&k->lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  struct kmem *k;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  push_off();

  k = &kmems[cpuid()];

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&k->lock);
  r->next = k->freelist;
  k->freelist = r;
  release(&k->lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  struct kmem *k;

  push_off();

  k = &kmems[cpuid()];

  acquire(&k->lock);
  r = k->freelist;

  if(r == 0)
    steal();

  r = k->freelist;

  if(r)
    k->freelist = r->next;

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk

  release(&k->lock);

  pop_off();

  return (void*)r;
}
