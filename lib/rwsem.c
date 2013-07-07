/* rwsem.c: R/W semaphores: contention handling functions
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from arch/i386/kernel/semaphore.c
 *
 * Writer lock-stealing by Alex Shi <alex.shi@intel.com>
 */
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/module.h>

/*
 * Initialize an rwsem:
 */
void __init_rwsem(struct rw_semaphore *sem, const char *name,
		  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held semaphore:
	 */
	debug_check_no_locks_freed((void *)sem, sizeof(*sem));
	lockdep_init_map(&sem->dep_map, name, key, 0);
#endif
	sem->count = RWSEM_UNLOCKED_VALUE;
	spin_lock_init(&sem->wait_lock);
	INIT_LIST_HEAD(&sem->wait_list);
}

EXPORT_SYMBOL(__init_rwsem);

struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	unsigned int flags;
#define RWSEM_WAITING_FOR_READ	0x00000001
#define RWSEM_WAITING_FOR_WRITE	0x00000002
};

/*
 * handle the lock release when processes blocked on it that can now run
 * - if we come here from up_xxxx(), then:
 *   - the 'active part' of count (&0x0000ffff) reached 0 (but may have changed)
 *   - the 'waiting part' of count (&0xffff0000) is -ve (and will still be so)
 *   - there must be someone on the queue
 * - the spinlock must be held by the caller
 * - woken process blocks are discarded from the list after having task zeroed
 * - writers are only woken if downgrading is false
 */
static inline struct rw_semaphore *
__rwsem_do_wake(struct rw_semaphore *sem, int downgrading)
{
	struct rwsem_waiter *waiter;
	struct task_struct *tsk;
	struct list_head *next;
	signed long woken, loop, adjustment;

	if (downgrading)
		goto dont_wake_writers;

	/* Wake up the writing waiter and let the task grab the sem: */
	wake_up_process(waiter->task);
	goto out;

	/* don't want to wake any writers */
 dont_wake_writers:
	waiter = list_entry(sem->wait_list.next, struct rwsem_waiter, list);
	if (waiter->flags & RWSEM_WAITING_FOR_WRITE)
		goto out;

	/* grant an infinite number of read locks to the readers at the front
	 * of the queue
	 * - note we increment the 'active part' of the count by the number of
	 *   readers before waking any processes up
	 */
 readers_only:
	woken = 0;
	do {
		woken++;

		if (waiter->list.next == &sem->wait_list)
			break;

		waiter = list_entry(waiter->list.next,
					struct rwsem_waiter, list);

	} while (waiter->flags & RWSEM_WAITING_FOR_READ);

	loop = woken;
	woken *= RWSEM_ACTIVE_BIAS - RWSEM_WAITING_BIAS;
	if (!downgrading)
		/* we'd already done one increment earlier */
		woken -= RWSEM_ACTIVE_BIAS;

	rwsem_atomic_add(woken, sem);

	next = sem->wait_list.next;
	for (; loop > 0; loop--) {
		waiter = list_entry(next, struct rwsem_waiter, list);
		next = waiter->list.next;
		tsk = waiter->task;
		smp_mb();
		waiter->task = NULL;
		wake_up_process(tsk);
		put_task_struct(tsk);
	}

	sem->wait_list.next = next;
	next->prev = &sem->wait_list;

 out:
	return sem;
}

/* Try to get write sem, caller holds sem->wait_lock: */
static int try_get_writer_sem(struct rw_semaphore *sem,
					struct rwsem_waiter *waiter)
{
	struct rwsem_waiter *fwaiter;
	long oldcount, adjustment;

	/* only steal when first waiter is writing */
	fwaiter = list_entry(sem->wait_list.next, struct rwsem_waiter, list);
	if (!(fwaiter->flags & RWSEM_WAITING_FOR_WRITE))
		return 0;

	adjustment = RWSEM_ACTIVE_WRITE_BIAS;
	/* Only one waiter in the queue: */
	if (fwaiter == waiter && waiter->list.next == &sem->wait_list)
		adjustment -= RWSEM_WAITING_BIAS;

try_again_write:
	oldcount = rwsem_atomic_update(adjustment, sem) - adjustment;
	if (!(oldcount & RWSEM_ACTIVE_MASK)) {
		/* No active lock: */
		struct task_struct *tsk = waiter->task;

		list_del(&waiter->list);
		smp_mb();
		put_task_struct(tsk);
		tsk->state = TASK_RUNNING;
		return 1;
	}
	/* some one grabbed the sem already */
	if (rwsem_atomic_update(-adjustment, sem) & RWSEM_ACTIVE_MASK)
		return 0;
	goto try_again_write;
}

/*
 * wait for a lock to be granted
 */
static struct rw_semaphore __sched *
rwsem_down_failed_common(struct rw_semaphore *sem,
			struct rwsem_waiter *waiter, signed long adjustment)
{
	struct task_struct *tsk = current;
	signed long count;

	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	/* set up my own style of waitqueue */
	spin_lock_irq(&sem->wait_lock);
	waiter->task = tsk;
	get_task_struct(tsk);

	list_add_tail(&waiter->list, &sem->wait_list);

	/* we're now waiting on the lock, but no longer actively read-locking */
	count = rwsem_atomic_update(adjustment, sem);

	/* if there are no active locks, wake the front queued process(es) up */
	if (!(count & RWSEM_ACTIVE_MASK))
		sem = __rwsem_do_wake(sem, 0);

	spin_unlock_irq(&sem->wait_lock);

	/* wait to be given the lock */
	for (;;) {
		if (!waiter->task)
			break;

		raw_spin_lock_irq(&sem->wait_lock);
		/* Try to get the writer sem, may steal from the head writer: */
		if (flags == RWSEM_WAITING_FOR_WRITE)
			if (try_get_writer_sem(sem, &waiter)) {
				raw_spin_unlock_irq(&sem->wait_lock);
				return sem;
			}
		raw_spin_unlock_irq(&sem->wait_lock);
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	tsk->state = TASK_RUNNING;

	return sem;
}

/*
 * wait for the read lock to be granted
 */
asmregparm struct rw_semaphore __sched *
rwsem_down_read_failed(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;

	waiter.flags = RWSEM_WAITING_FOR_READ;
	rwsem_down_failed_common(sem, &waiter,
				RWSEM_WAITING_BIAS - RWSEM_ACTIVE_BIAS);
	return sem;
}

/*
 * wait for the write lock to be granted
 */
asmregparm struct rw_semaphore __sched *
rwsem_down_write_failed(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;

	waiter.flags = RWSEM_WAITING_FOR_WRITE;
	rwsem_down_failed_common(sem, &waiter, -RWSEM_ACTIVE_BIAS);

	return sem;
}

/*
 * handle waking up a waiter on the semaphore
 * - up_read/up_write has decremented the active part of count if we come here
 */
asmregparm struct rw_semaphore *rwsem_wake(struct rw_semaphore *sem)
{
	unsigned long flags;

	spin_lock_irqsave(&sem->wait_lock, flags);

	/* do nothing if list empty */
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 0);

	spin_unlock_irqrestore(&sem->wait_lock, flags);

	return sem;
}

/*
 * downgrade a write lock into a read lock
 * - caller incremented waiting part of count and discovered it still negative
 * - just wake up any readers at the front of the queue
 */
asmregparm struct rw_semaphore *rwsem_downgrade_wake(struct rw_semaphore *sem)
{
	unsigned long flags;

	spin_lock_irqsave(&sem->wait_lock, flags);

	/* do nothing if list empty */
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 1);

	spin_unlock_irqrestore(&sem->wait_lock, flags);

	return sem;
}

EXPORT_SYMBOL(rwsem_down_read_failed);
EXPORT_SYMBOL(rwsem_down_write_failed);
EXPORT_SYMBOL(rwsem_wake);
EXPORT_SYMBOL(rwsem_downgrade_wake);
