// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"


struct bucket {
  struct spinlock lock;
  struct buf head;
};

struct {
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;
  struct bucket *bucket;

  for(bucket = bcache.bucket; bucket < bcache.bucket+NBUCKET; bucket++){
    initlock(&bucket->lock, "bcache");
    bucket->head.prev = &bucket->head;
    bucket->head.next = &bucket->head;
  }

  bucket = bcache.bucket;

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    initsleeplock(&b->lock, "buffer");
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct bucket *bucket, *p;

  bucket = &bcache.bucket[blockno % NBUCKET];

  acquire(&bucket->lock);

  // Is the block already cached?
  for(b = bucket->head.next; b != &bucket->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bucket->head.next; b != &bucket->head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  for(p = bcache.bucket; p < bcache.bucket+NBUCKET; p++){
    if(p == bucket)
      continue;
    acquire(&p->lock);
    for(b = p->head.next; b != &p->head; b = b->next){
      if(b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        b->next->prev = b->prev;
        b->prev->next = b->next;
        release(&p->lock);
        b->next = bucket->head.next;
        b->prev = &bucket->head;
        bucket->head.next->prev = b;
        bucket->head.next = b;
        release(&bucket->lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&p->lock);
  }
  release(&bucket->lock);

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  struct bucket *bucket = &bcache.bucket[b->blockno % NBUCKET];

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bucket->head.next;
    b->prev = &bucket->head;
    bucket->head.next->prev = b;
    bucket->head.next = b;
  }

  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  struct bucket *bucket = &bcache.bucket[b->blockno % NBUCKET];
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  struct bucket *bucket = &bcache.bucket[b->blockno % NBUCKET];
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}
