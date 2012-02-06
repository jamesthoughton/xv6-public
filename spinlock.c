// Mutual exclusion spin locks.

#include "types.h"
#include "kernel.h"
#include "amd64.h"
#include "cpu.h"
#include "bits.h"
#include "spinlock.h"
#include "mtrace.h"
#include "condvar.h"
#include "fs.h"
#include "file.h"

#if LOCKSTAT
static int lockstat_enable;
#endif

static inline void
locking(struct spinlock *lk)
{
#if SPINLOCK_DEBUG
  if(holding(lk)) {
    cprintf("%p\n", __builtin_return_address(0));
    panic("acquire");
  }
#endif

  mtlock(lk);
}

static inline void
locked(struct spinlock *lk)
{
  mtacquired(lk);
  
#if SPINLOCK_DEBUG
  // Record info about lock acquisition for debugging.
  lk->cpu = mycpu();
  getcallerpcs(&lk, lk->pcs, NELEM(lk->pcs));
#endif

#if LOCKSTAT
  if (lockstat_enable && lk->stat != NULL) {
    lk->stat->s.cpu[cpunum()].acquires++;
  }
#endif
}

static inline void
releasing(struct spinlock *lk)
{
#if SPINLOCK_DEBUG
  if(!holding(lk)) {
    cprintf("lock: %s\n", lk->name);
    panic("release");
  }
#endif

  mtunlock(lk);

#if SPINLOCK_DEBUG
  lk->pcs[0] = 0;
  lk->cpu = 0;
#endif
}

// Check whether this cpu is holding the lock.
#if SPINLOCK_DEBUG
int
holding(struct spinlock *lock)
{
  return lock->locked && lock->cpu == mycpu();
}
#endif

void
initlock(struct spinlock *lk, const char *name)
{
#if SPINLOCK_DEBUG
  lk->name = name;
  lk->cpu = 0;
#endif
#if LOCKSTAT
  lk->stat = NULL;
#endif
  lk->locked = 0;
}

int
tryacquire(struct spinlock *lk)
{
  pushcli();
  locking(lk);
  if (xchg32(&lk->locked, 1) != 0) {
      popcli();
      return 0;
  }
  locked(lk);
  return 1;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
// Holding a lock for a long time may cause
// other CPUs to waste time spinning to acquire it.
void
acquire(struct spinlock *lk)
{
  pushcli();
  locking(lk);
  // The xchg is atomic.
  // It also serializes, so that reads after acquire are not
  // reordered before it.
  while(xchg32(&lk->locked, 1) != 0)
    ;
  locked(lk);
}

// Release the lock.
void
release(struct spinlock *lk)
{
  releasing(lk);

  // The xchg serializes, so that reads before release are
  // not reordered after it.  The 1996 PentiumPro manual (Volume 3,
  // 7.2) says reads can be carried out speculatively and in
  // any order, which implies we need to serialize here.
  // But the 2007 Intel 64 Architecture Memory Ordering White
  // Paper says that Intel 64 and IA-32 will not move a load
  // after a store. So lock->locked = 0 would work here.
  // The xchg being asm volatile ensures gcc emits it after
  // the above assignments (and after the critical section).
  xchg32(&lk->locked, 0);

  popcli();
}

#if LOCKSTAT

LIST_HEAD(lockstat_list, klockstat);
static struct lockstat_list lockstat_list = LIST_HEAD_INITIALIZER(lockstat_list);
static struct spinlock lockstat_lock = { 
  .locked = 0,
#if SPINLOCK_DEBUG
  .name = "lockstat",
  .cpu = NULL,
#endif
};

void
lockstat_init(struct spinlock *lk)
{
  if (lk->stat != NULL)
    panic("initlockstat");

  lk->stat = kmalloc(sizeof(*lk->stat));
  if (lk->stat == NULL)
    return;
  memset(lk->stat, 0, sizeof(*lk->stat));

  lk->stat->active = 1;
  safestrcpy(lk->stat->s.name, lk->name, sizeof(lk->stat->s.name));

  acquire(&lockstat_lock);
  LIST_INSERT_HEAD(&lockstat_list, lk->stat, link);
  release(&lockstat_lock);
}

void
lockstat_stop(struct spinlock *lk)
{
  if (lk->stat != NULL) {
    lk->stat->active = 0;
    lk->stat = NULL;
  }
}

void
lockstat_clear(void)
{
  struct klockstat *stat, *tmp;

  acquire(&lockstat_lock);
  LIST_FOREACH_SAFE(stat, &lockstat_list, link, tmp) {
    if (stat->active == 0) {
      LIST_REMOVE(stat, link);
      kmfree(stat);
    } else {
      memset(&stat->s.cpu, 0, sizeof(stat->s.cpu));
    }
  }
  release(&lockstat_lock);
}

static int
lockstat_read(struct inode *ip, char *dst, u32 off, u32 n)
{
  static const u64 sz = sizeof(struct lockstat);
  struct klockstat *stat;
  u32 cur;

  if (off % sz || n < sz)
    return -1;

  cur = 0;
  acquire(&lockstat_lock);
  LIST_FOREACH(stat, &lockstat_list, link) {
    struct lockstat *ls = &stat->s;
    if (n < sizeof(*ls))
      break;
    if (cur >= off) {
      memmove(dst, ls, sz);
      dst += sz;
      n -= sz;
    }
    cur += sz;
  }
  release(&lockstat_lock);

  return cur >= off ? cur - off : 0;
}

static int
lockstat_write(struct inode *ip, char *buf, u32 off, u32 n)
{
  int cmd = buf[0] - '0';

  switch(cmd) {
  case LOCKSTAT_START:
    lockstat_enable = 1;
    break;
  case LOCKSTAT_STOP:
    lockstat_enable = 0;
    break;
  case LOCKSTAT_CLEAR:
    lockstat_clear();
    break;
  default:
    return -1;
  }
  return n;
}

void
initlockstat(void)
{
  devsw[DEVLOCKSTAT].write = lockstat_write;
  devsw[DEVLOCKSTAT].read = lockstat_read;
}

#else

void
lockstat_init(struct spinlock *lk)
{
}

void
lockstat_stop(struct spinlock *lk)
{
}

void
initlockstat(void)
{
}
#endif
