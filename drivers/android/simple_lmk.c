// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/sched/mm.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <linux/vmpressure.h>
#include <uapi/linux/sched/types.h>

/* The minimum number of pages to free per reclaim */
#define MIN_FREE_PAGES (CONFIG_ANDROID_SIMPLE_LMK_MINFREE * SZ_1M / PAGE_SIZE)

/* Kill up to this many victims per reclaim */
#define MAX_VICTIMS 1024

/* Timeout in jiffies for each reclaim */
#define RECLAIM_EXPIRES msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC)

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;
};

static struct victim_info victims[MAX_VICTIMS] __cacheline_aligned_in_smp;
static struct task_struct *task_bucket[SHRT_MAX + 1] __cacheline_aligned;
static DECLARE_WAIT_QUEUE_HEAD(oom_waitq);
static DECLARE_WAIT_QUEUE_HEAD(reaper_waitq);
static DECLARE_COMPLETION(reclaim_done);
static __cacheline_aligned_in_smp DEFINE_RWLOCK(mm_free_lock);
static int nr_victims;
static bool reclaim_active;
static atomic_t needs_reclaim = ATOMIC_INIT(0);
static atomic_t needs_reap = ATOMIC_INIT(0);
static atomic_t nr_killed = ATOMIC_INIT(0);

static int victim_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	return rhs->size - lhs->size;
}

static void victim_swap(void *lhs_ptr, void *rhs_ptr, int size)
{
	struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	swap(*lhs, *rhs);
}

static unsigned long get_total_mm_pages(struct mm_struct *mm)
{
	unsigned long pages = 0;
	int i;

	for (i = 0; i < NR_MM_COUNTERS; i++)
		pages += get_mm_counter(mm, i);

	return pages;
}

static unsigned long find_victims(int *vindex)
{
	short i, min_adj = SHRT_MAX, max_adj = 0;
	unsigned long pages_found = 0;
	struct task_struct *tsk;

	rcu_read_lock();
	for_each_process(tsk) {
		struct signal_struct *sig;
		short adj;

		/*
		 * Search for suitable tasks with a positive adj (importance).
		 * Since only tasks with a positive adj can be targeted, that
		 * naturally excludes tasks which shouldn't be killed, like init
		 * and kthreads. Although oom_score_adj can still be changed
		 * while this code runs, it doesn't really matter; we just need
		 * a snapshot of the task's adj.
		 */
		sig = tsk->signal;
		adj = READ_ONCE(sig->oom_score_adj);
		if (adj < 0 || sig->flags & SIGNAL_GROUP_EXIT ||
		    (thread_group_empty(tsk) && tsk->flags & PF_EXITING))
			continue;

		/* Store the task in a linked-list bucket based on its adj */
		tsk->simple_lmk_next = task_bucket[adj];
		task_bucket[adj] = tsk;

		/* Track the min and max adjs to speed up the loop below */
		if (adj > max_adj)
			max_adj = adj;
		if (adj < min_adj)
			min_adj = adj;
	}

	/* Start searching for victims from the highest adj (least important) */
	for (i = max_adj; i >= min_adj; i--) {
		int old_vindex;

		tsk = task_bucket[i];
		if (!tsk)
			continue;

		/* Clear out this bucket for the next time reclaim is done */
		task_bucket[i] = NULL;

		/* Iterate through every task with this adj */
		old_vindex = *vindex;
		do {
			struct task_struct *vtsk;

			vtsk = find_lock_task_mm(tsk);
			if (!vtsk)
				continue;

			/* Store this potential victim away for later */
			victims[*vindex].tsk = vtsk;
			victims[*vindex].mm = vtsk->mm;
			victims[*vindex].size = get_total_mm_pages(vtsk->mm);

			/* Count the number of pages that have been found */
			pages_found += victims[*vindex].size;

			/* Make sure there's space left in the victim array */
			if (++*vindex == MAX_VICTIMS)
				break;
		} while ((tsk = tsk->simple_lmk_next));

		/* Go to the next bucket if nothing was found */
		if (*vindex == old_vindex)
			continue;

		/*
		 * Sort the victims in descending order of size to prioritize
		 * killing the larger ones first.
		 */
		sort(&victims[old_vindex], *vindex - old_vindex,
		     sizeof(*victims), victim_cmp, victim_swap);

		/* Stop when we are out of space or have enough pages found */
		if (*vindex == MAX_VICTIMS || pages_found >= MIN_FREE_PAGES) {
			/* Zero out any remaining buckets we didn't touch */
			if (i > min_adj)
				memset(&task_bucket[min_adj], 0,
				       (i - min_adj) * sizeof(*task_bucket));
			break;
		}
	}
	rcu_read_unlock();

	return pages_found;
}

static int process_victims(int vlen)
{
	unsigned long pages_found = 0;
	int i, nr_to_kill = 0;

	/*
	 * Calculate the number of tasks that need to be killed and quickly
	 * release the references to those that'll live.
	 */
	for (i = 0; i < vlen; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		/* The victim's mm lock is taken in find_victims; release it */
		if (pages_found >= MIN_FREE_PAGES) {
			task_unlock(vtsk);
		} else {
			pages_found += victim->size;
			nr_to_kill++;
		}
	}

	return nr_to_kill;
}

static void set_task_rt_prio(struct task_struct *tsk, int priority)
{
	const struct sched_param rt_prio = {
		.sched_priority = priority
	};

	sched_setscheduler_nocheck(tsk, SCHED_RR, &rt_prio);
}

static void scan_and_kill(void)
{
	int i, nr_to_kill, nr_found = 0;
	unsigned long pages_found;

	/*
	 * Reset nr_victims so the reaper thread and simple_lmk_mm_freed() are
	 * aware that the victims array is no longer valid.
	 */
	write_lock(&mm_free_lock);
	nr_victims = 0;
	write_unlock(&mm_free_lock);

	/* Populate the victims array with tasks sorted by adj and then size */
	pages_found = find_victims(&nr_found);
	if (unlikely(!nr_found)) {
		pr_err_ratelimited("No processes available to kill!\n");
		return;
	}

	/* Minimize the number of victims if we found more pages than needed */
	if (pages_found > MIN_FREE_PAGES) {
		/* First round of processing to weed out unneeded victims */
		nr_to_kill = process_victims(nr_found);

		/*
		 * Try to kill as few of the chosen victims as possible by
		 * sorting the chosen victims by size, which means larger
		 * victims that have a lower adj can be killed in place of
		 * smaller victims with a high adj.
		 */
		sort(victims, nr_to_kill, sizeof(*victims), victim_cmp,
		     victim_swap);

		/* Second round of processing to finally select the victims */
		nr_to_kill = process_victims(nr_to_kill);
	} else {
		/* Too few pages found, so all the victims need to be killed */
		nr_to_kill = nr_found;
	}

	/*
	 * Store the final number of victims for simple_lmk_mm_freed() and the
	 * reaper thread, and indicate that reclaim is active.
	 */
	write_lock(&mm_free_lock);
	nr_victims = nr_to_kill;
	reclaim_active = true;
	write_unlock(&mm_free_lock);

	/* Kill the victims */
	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *t, *vtsk = victim->tsk;
		struct mm_struct *mm = victim->mm;

		pr_info("Killing %s with adj %d to free %lu KiB\n", vtsk->comm,
			vtsk->signal->oom_score_adj,
			victim->size << (PAGE_SHIFT - 10));

		/* Accelerate the victim's death by forcing the kill signal */
		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, vtsk, PIDTYPE_TGID);

		/*
		 * Mark the thread group dead so that other kernel code knows,
		 * and then elevate the thread group to SCHED_RR with minimum RT
		 * priority. The entire group needs to be elevated because
		 * there's no telling which threads have references to the mm as
		 * well as which thread will happen to put the final reference
		 * and release the mm's memory. If the mm is released from a
		 * thread with low scheduling priority then it may take a very
		 * long time for exit_mmap() to complete.
		 */
		rcu_read_lock();
		for_each_thread(vtsk, t)
			set_tsk_thread_flag(t, TIF_MEMDIE);
		for_each_thread(vtsk, t)
			set_task_rt_prio(t, 1);
		rcu_read_unlock();

		/* Allow the victim to run on any CPU. This won't schedule. */
		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		/* Signals can't wake frozen tasks; only a thaw operation can */
		__thaw_task(vtsk);

		/* Store the number of anon pages to sort victims for reaping */
		victim->size = get_mm_counter(mm, MM_ANONPAGES);

		/* Finally release the victim's task lock acquired earlier */
		task_unlock(vtsk);
	}

	/*
	 * Sort the victims by descending order of anonymous pages so the reaper
	 * thread can prioritize reaping the victims with the most anonymous
	 * pages first. Then wake the reaper thread if it's asleep. The lock
	 * orders the needs_reap store before waitqueue_active().
	 */
	write_lock(&mm_free_lock);
	sort(victims, nr_to_kill, sizeof(*victims), victim_cmp, victim_swap);
	atomic_set(&needs_reap, 1);
	write_unlock(&mm_free_lock);
	if (waitqueue_active(&reaper_waitq))
		wake_up(&reaper_waitq);

	/* Wait until all the victims die or until the timeout is reached */
	if (!wait_for_completion_timeout(&reclaim_done, RECLAIM_EXPIRES))
		pr_info("Timeout hit waiting for victims to die, proceeding\n");

	/* Clean up for future reclaims but let the reaper thread keep going */
	write_lock(&mm_free_lock);
	reinit_completion(&reclaim_done);
	reclaim_active = false;
	nr_killed = (atomic_t)ATOMIC_INIT(0);
	write_unlock(&mm_free_lock);
}

static int simple_lmk_reclaim_thread(void *data)
{
	/* Use maximum RT priority */
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		wait_event_freezable(oom_waitq, atomic_read(&needs_reclaim));
		scan_and_kill();
		atomic_set(&needs_reclaim, 0);
	}

	return 0;
}

static struct mm_struct *next_reap_victim(void)
{
	struct mm_struct *mm = NULL;
	bool should_retry = false;
	int i;

	/* Take a write lock so no victim's mm can be freed while scanning */
	write_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++, mm = NULL) {
		/* Check if this victim is alive and hasn't been reaped yet */
		mm = victims[i].mm;
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		/* Do a trylock so the reaper thread doesn't sleep */
		if (!mmap_read_trylock(mm)) {
			should_retry = true;
			continue;
		}

		/*
		 * Check MMF_OOM_SKIP again under the lock in case this mm was
		 * reaped by exit_mmap() and then had its page tables destroyed.
		 * No mmgrab() is needed because the reclaim thread sets
		 * MMF_OOM_VICTIM under task_lock() for the mm's task, which
		 * guarantees that MMF_OOM_VICTIM is always set before the
		 * victim mm can enter exit_mmap(). Therefore, an mmap read lock
		 * is sufficient to keep the mm struct itself from being freed.
		 */
		if (!test_bit(MMF_OOM_SKIP, &mm->flags))
			break;
		mmap_read_unlock(mm);
	}

	if (!mm) {
		if (should_retry)
			/* Return ERR_PTR(-EAGAIN) to try reaping again later */
			mm = ERR_PTR(-EAGAIN);
		else if (!reclaim_active)
			/*
			 * Nothing left to reap, so stop simple_lmk_mm_freed()
			 * from iterating over the victims array since reclaim
			 * is no longer active. Return NULL to stop reaping.
			 */
			nr_victims = 0;
	}
	write_unlock(&mm_free_lock);

	return mm;
}

static void reap_victims(void)
{
	struct mm_struct *mm;

	while ((mm = next_reap_victim())) {
		if (IS_ERR(mm)) {
			/* Wait one jiffy before trying to reap again */
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/*
		 * Try to reap the victim. Mark it as reaped with MMF_OOM_SKIP
		 * if successful.
		 */
		if (__oom_reap_task_mm(mm))
			set_bit(MMF_OOM_SKIP, &mm->flags);
		mmap_read_unlock(mm);
	}
}

static int simple_lmk_reaper_thread(void *data)
{
	/* Use a lower priority than the reclaim thread */
	set_task_rt_prio(current, MAX_RT_PRIO - 2);
	set_freezable();

	while (1) {
		wait_event_freezable(reaper_waitq,
				     atomic_cmpxchg_relaxed(&needs_reap, 1, 0));
		reap_victims();
	}

	return 0;
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	int i;

	/*
	 * Victims are guaranteed to have MMF_OOM_SKIP set after exit_mmap()
	 * finishes. Use this to ignore unrelated dying processes.
	 */
	if (!test_bit(MMF_OOM_SKIP, &mm->flags))
		return;

	read_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++) {
		if (victims[i].mm == mm) {
			/*
			 * Clear out this victim from the victims array and only
			 * increment nr_killed if reclaim is active. If reclaim
			 * isn't active, then clearing out the victim is done
			 * solely for the reaper thread to avoid freed victims.
			 */
			victims[i].mm = NULL;
			if (reclaim_active &&
			    atomic_inc_return_relaxed(&nr_killed) == nr_victims)
				complete(&reclaim_done);
			break;
		}
	}
	read_unlock(&mm_free_lock);
}

static int simple_lmk_vmpressure_cb(struct notifier_block *nb,
				    unsigned long pressure, void *data)
{
	if (pressure == 100) {
		atomic_set(&needs_reclaim, 1);
		smp_mb__after_atomic();
		if (waitqueue_active(&oom_waitq))
			wake_up(&oom_waitq);
	}

	return NOTIFY_OK;
}

static struct notifier_block vmpressure_notif = {
	.notifier_call = simple_lmk_vmpressure_cb,
	.priority = INT_MAX
};

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);
	struct task_struct *thread;

	if (!atomic_cmpxchg(&init_done, 0, 1)) {
		thread = kthread_run(simple_lmk_reaper_thread, NULL,
				     "simple_lmkd_reaper");
		BUG_ON(IS_ERR(thread));
		thread = kthread_run(simple_lmk_reclaim_thread, NULL,
				     "simple_lmkd");
		BUG_ON(IS_ERR(thread));
		BUG_ON(vmpressure_notifier_register(&vmpressure_notif));
	}

	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);

#ifdef CONFIG_PROC_FS
/*
 * lmkd switches to its in-kernel LMK mode when it finds the minfree parameter
 * above, but ActivityManager still tells it to start PSI monitoring once boot
 * completes, and lmkd exits when it can't, since Simple LMK needs PSI off.
 * Give it a /proc/pressure/memory that takes triggers like PSI's but never
 * reports a stall, so lmkd stays up without becoming a second LMK.
 */
#define PSI_COMPAT_WINDOW_MAX_US (10 * USEC_PER_SEC)

#ifdef CONFIG_PSI
/* PSI built in (Android's VINTF requires it) but off unless psi=1 is passed */
extern struct static_key_false psi_disabled;
#endif

static int psi_compat_show(struct seq_file *m, void *v)
{
	seq_puts(m, "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
		    "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
	return 0;
}

static int psi_compat_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_compat_show, NULL);
}

static ssize_t psi_compat_write(struct file *file, const char __user *ubuf,
				size_t nbytes, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	unsigned int threshold_us, window_us;
	char buf[32];
	size_t len;

	if (!nbytes)
		return -EINVAL;

	len = min(nbytes, sizeof(buf));
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len - 1] = '\0';

	/* Validate the trigger the same way PSI does */
	if (sscanf(buf, "some %u %u", &threshold_us, &window_us) != 2 &&
	    sscanf(buf, "full %u %u", &threshold_us, &window_us) != 2)
		return -EINVAL;
	if (!window_us || window_us > PSI_COMPAT_WINDOW_MAX_US ||
	    !threshold_us || threshold_us > window_us)
		return -EINVAL;

	/* Like PSI, allow only one trigger per file descriptor */
	mutex_lock(&seq->lock);
	if (seq->private) {
		mutex_unlock(&seq->lock);
		return -EBUSY;
	}
	/* Any non-NULL value marks the trigger as set */
	smp_store_release(&seq->private, file);
	mutex_unlock(&seq->lock);

	return nbytes;
}

static __poll_t psi_compat_poll(struct file *file, poll_table *wait)
{
	struct seq_file *seq = file->private_data;

	/* Like PSI, polling without a trigger is an error */
	if (!smp_load_acquire(&seq->private))
		return DEFAULT_POLLMASK | EPOLLERR | EPOLLPRI;

	/* The trigger never fires */
	return DEFAULT_POLLMASK;
}

static const struct proc_ops psi_compat_proc_ops = {
	.proc_open	= psi_compat_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_compat_write,
	.proc_poll	= psi_compat_poll,
	.proc_release	= single_release,
};

static int __init psi_compat_init(void)
{
#ifdef CONFIG_PSI
	if (!static_branch_likely(&psi_disabled)) {
		pr_warn("PSI is enabled, lmkd will kill alongside Simple LMK\n");
		return 0;
	}
#endif
	if (!proc_mkdir("pressure", NULL) ||
	    !proc_create("pressure/memory", 0, NULL, &psi_compat_proc_ops))
		return -ENOMEM;

	return 0;
}
late_initcall(psi_compat_init);
#endif
