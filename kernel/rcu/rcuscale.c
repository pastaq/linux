// SPDX-License-Identifier: GPL-2.0+
/*
 * Read-Copy Update module-based scalability-test facility
 *
 * Copyright (C) IBM Corporation, 2015
 *
 * Authors: Paul E. McKenney <paulmck@linux.ibm.com>
 */

#define pr_fmt(fmt) fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/rcupdate.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/freezer.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/stat.h>
#include <linux/srcu.h>
#include <linux/slab.h>
#include <asm/byteorder.h>
#include <linux/torture.h>
#include <linux/vmalloc.h>
#include <linux/rcupdate_trace.h>
#include <linux/sched/debug.h>

#include "rcu.h"

MODULE_DESCRIPTION("Read-Copy Update module-based scalability-test facility");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paul E. McKenney <paulmck@linux.ibm.com>");

#define SCALE_FLAG "-scale:"
#define SCALEOUT_STRING(s) \
	pr_alert("%s" SCALE_FLAG " %s\n", scale_type, s)
#define VERBOSE_SCALEOUT_STRING(s) \
	do { if (verbose) pr_alert("%s" SCALE_FLAG " %s\n", scale_type, s); } while (0)
#define SCALEOUT_ERRSTRING(s) \
	pr_alert("%s" SCALE_FLAG "!!! %s\n", scale_type, s)

/*
 * The intended use cases for the nreaders and nwriters module parameters
 * are as follows:
 *
 * 1.	Specify only the nr_cpus kernel boot parameter.  This will
 *	set both nreaders and nwriters to the value specified by
 *	nr_cpus for a mixed reader/writer test.
 *
 * 2.	Specify the nr_cpus kernel boot parameter, but set
 *	rcuscale.nreaders to zero.  This will set nwriters to the
 *	value specified by nr_cpus for an update-only test.
 *
 * 3.	Specify the nr_cpus kernel boot parameter, but set
 *	rcuscale.nwriters to zero.  This will set nreaders to the
 *	value specified by nr_cpus for a read-only test.
 *
 * Various other use cases may of course be specified.
 *
 * Note that this test's readers are intended only as a test load for
 * the writers.  The reader scalability statistics will be overly
 * pessimistic due to the per-critical-section interrupt disabling,
 * test-end checks, and the pair of calls through pointers.
 */

#ifdef MODULE
# define RCUSCALE_SHUTDOWN 0
#else
# define RCUSCALE_SHUTDOWN 1
#endif

torture_param(bool, gp_async, false, "Use asynchronous GP wait primitives");
torture_param(int, gp_async_max, 1000, "Max # outstanding waits per writer");
torture_param(bool, gp_exp, false, "Use expedited GP wait primitives");
torture_param(int, holdoff, 10, "Holdoff time before test start (s)");
torture_param(int, minruntime, 0, "Minimum run time (s)");
torture_param(int, nreaders, -1, "Number of RCU reader threads");
torture_param(int, nwriters, -1, "Number of RCU updater threads");
torture_param(bool, shutdown, RCUSCALE_SHUTDOWN,
	      "Shutdown at end of scalability tests.");
torture_param(int, verbose, 1, "Enable verbose debugging printk()s");
torture_param(int, writer_holdoff, 0, "Holdoff (us) between GPs, zero to disable");
torture_param(int, writer_holdoff_jiffies, 0, "Holdoff (jiffies) between GPs, zero to disable");
torture_param(int, kfree_rcu_test, 0, "Do we run a kfree_rcu() scale test?");
torture_param(int, kfree_mult, 1, "Multiple of kfree_obj size to allocate.");
torture_param(int, kfree_by_call_rcu, 0, "Use call_rcu() to emulate kfree_rcu()?");

static char *scale_type = "rcu";
module_param(scale_type, charp, 0444);
MODULE_PARM_DESC(scale_type, "Type of RCU to scalability-test (rcu, srcu, ...)");

// Structure definitions for custom fixed-per-task allocator.
struct writer_mblock {
	struct rcu_head wmb_rh;
	struct llist_node wmb_node;
	struct writer_freelist *wmb_wfl;
};

struct writer_freelist {
	struct llist_head ws_lhg;
	atomic_t ws_inflight;
	struct llist_head ____cacheline_internodealigned_in_smp ws_lhp;
	struct writer_mblock *ws_mblocks;
};

static int nrealreaders;
static int nrealwriters;
static struct task_struct **writer_tasks;
static struct task_struct **reader_tasks;
static struct task_struct *shutdown_task;

static u64 **writer_durations;
static bool *writer_done;
static struct writer_freelist *writer_freelists;
static int *writer_n_durations;
static atomic_t n_rcu_scale_reader_started;
static atomic_t n_rcu_scale_writer_started;
static atomic_t n_rcu_scale_writer_finished;
static wait_queue_head_t shutdown_wq;
static u64 t_rcu_scale_writer_started;
static u64 t_rcu_scale_writer_finished;
static unsigned long b_rcu_gp_test_started;
static unsigned long b_rcu_gp_test_finished;

#define MAX_MEAS 10000
#define MIN_MEAS 100

/*
 * Operations vector for selecting different types of tests.
 */

struct rcu_scale_ops {
	int ptype;
	void (*init)(void);
	void (*cleanup)(void);
	int (*readlock)(void);
	void (*readunlock)(int idx);
	unsigned long (*get_gp_seq)(void);
	unsigned long (*gp_diff)(unsigned long new, unsigned long old);
	unsigned long (*exp_completed)(void);
	void (*async)(struct rcu_head *head, rcu_callback_t func);
	void (*gp_barrier)(void);
	void (*sync)(void);
	void (*exp_sync)(void);
	struct task_struct *(*rso_gp_kthread)(void);
	void (*stats)(void);
	const char *name;
};

static struct rcu_scale_ops *cur_ops;

/*
 * Definitions for rcu scalability testing.
 */

static int rcu_scale_read_lock(void) __acquires(RCU)
{
	rcu_read_lock();
	return 0;
}

static void rcu_scale_read_unlock(int idx) __releases(RCU)
{
	rcu_read_unlock();
}

static unsigned long __maybe_unused rcu_no_completed(void)
{
	return 0;
}

static void rcu_sync_scale_init(void)
{
}

static struct rcu_scale_ops rcu_ops = {
	.ptype		= RCU_FLAVOR,
	.init		= rcu_sync_scale_init,
	.readlock	= rcu_scale_read_lock,
	.readunlock	= rcu_scale_read_unlock,
	.get_gp_seq	= rcu_get_gp_seq,
	.gp_diff	= rcu_seq_diff,
	.exp_completed	= rcu_exp_batches_completed,
	.async		= call_rcu_hurry,
	.gp_barrier	= rcu_barrier,
	.sync		= synchronize_rcu,
	.exp_sync	= synchronize_rcu_expedited,
	.name		= "rcu"
};

/*
 * Definitions for srcu scalability testing.
 */

DEFINE_STATIC_SRCU(srcu_ctl_scale);
static struct srcu_struct *srcu_ctlp = &srcu_ctl_scale;

static int srcu_scale_read_lock(void) __acquires(srcu_ctlp)
{
	return srcu_read_lock(srcu_ctlp);
}

static void srcu_scale_read_unlock(int idx) __releases(srcu_ctlp)
{
	srcu_read_unlock(srcu_ctlp, idx);
}

static unsigned long srcu_scale_completed(void)
{
	return srcu_batches_completed(srcu_ctlp);
}

static void srcu_call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	call_srcu(srcu_ctlp, head, func);
}

static void srcu_rcu_barrier(void)
{
	srcu_barrier(srcu_ctlp);
}

static void srcu_scale_synchronize(void)
{
	synchronize_srcu(srcu_ctlp);
}

static void srcu_scale_stats(void)
{
	srcu_torture_stats_print(srcu_ctlp, scale_type, SCALE_FLAG);
}

static void srcu_scale_synchronize_expedited(void)
{
	synchronize_srcu_expedited(srcu_ctlp);
}

static struct rcu_scale_ops srcu_ops = {
	.ptype		= SRCU_FLAVOR,
	.init		= rcu_sync_scale_init,
	.readlock	= srcu_scale_read_lock,
	.readunlock	= srcu_scale_read_unlock,
	.get_gp_seq	= srcu_scale_completed,
	.gp_diff	= rcu_seq_diff,
	.exp_completed	= srcu_scale_completed,
	.async		= srcu_call_rcu,
	.gp_barrier	= srcu_rcu_barrier,
	.sync		= srcu_scale_synchronize,
	.exp_sync	= srcu_scale_synchronize_expedited,
	.stats		= srcu_scale_stats,
	.name		= "srcu"
};

static struct srcu_struct srcud;

static void srcu_sync_scale_init(void)
{
	srcu_ctlp = &srcud;
	init_srcu_struct(srcu_ctlp);
}

static void srcu_sync_scale_cleanup(void)
{
	cleanup_srcu_struct(srcu_ctlp);
}

static struct rcu_scale_ops srcud_ops = {
	.ptype		= SRCU_FLAVOR,
	.init		= srcu_sync_scale_init,
	.cleanup	= srcu_sync_scale_cleanup,
	.readlock	= srcu_scale_read_lock,
	.readunlock	= srcu_scale_read_unlock,
	.get_gp_seq	= srcu_scale_completed,
	.gp_diff	= rcu_seq_diff,
	.exp_completed	= srcu_scale_completed,
	.async		= srcu_call_rcu,
	.gp_barrier	= srcu_rcu_barrier,
	.sync		= srcu_scale_synchronize,
	.exp_sync	= srcu_scale_synchronize_expedited,
	.stats		= srcu_scale_stats,
	.name		= "srcud"
};

#ifdef CONFIG_TASKS_RCU

/*
 * Definitions for RCU-tasks scalability testing.
 */

static int tasks_scale_read_lock(void)
{
	return 0;
}

static void tasks_scale_read_unlock(int idx)
{
}

static void rcu_tasks_scale_stats(void)
{
	rcu_tasks_torture_stats_print(scale_type, SCALE_FLAG);
}

static struct rcu_scale_ops tasks_ops = {
	.ptype		= RCU_TASKS_FLAVOR,
	.init		= rcu_sync_scale_init,
	.readlock	= tasks_scale_read_lock,
	.readunlock	= tasks_scale_read_unlock,
	.get_gp_seq	= rcu_no_completed,
	.gp_diff	= rcu_seq_diff,
	.async		= call_rcu_tasks,
	.gp_barrier	= rcu_barrier_tasks,
	.sync		= synchronize_rcu_tasks,
	.exp_sync	= synchronize_rcu_tasks,
	.rso_gp_kthread	= get_rcu_tasks_gp_kthread,
	.stats		= IS_ENABLED(CONFIG_TINY_RCU) ? NULL : rcu_tasks_scale_stats,
	.name		= "tasks"
};

#define TASKS_OPS &tasks_ops,

#else // #ifdef CONFIG_TASKS_RCU

#define TASKS_OPS

#endif // #else // #ifdef CONFIG_TASKS_RCU

#ifdef CONFIG_TASKS_RUDE_RCU

/*
 * Definitions for RCU-tasks-rude scalability testing.
 */

static int tasks_rude_scale_read_lock(void)
{
	return 0;
}

static void tasks_rude_scale_read_unlock(int idx)
{
}

static void rcu_tasks_rude_scale_stats(void)
{
	rcu_tasks_rude_torture_stats_print(scale_type, SCALE_FLAG);
}

static struct rcu_scale_ops tasks_rude_ops = {
	.ptype		= RCU_TASKS_RUDE_FLAVOR,
	.init		= rcu_sync_scale_init,
	.readlock	= tasks_rude_scale_read_lock,
	.readunlock	= tasks_rude_scale_read_unlock,
	.get_gp_seq	= rcu_no_completed,
	.gp_diff	= rcu_seq_diff,
	.sync		= synchronize_rcu_tasks_rude,
	.exp_sync	= synchronize_rcu_tasks_rude,
	.rso_gp_kthread	= get_rcu_tasks_rude_gp_kthread,
	.stats		= IS_ENABLED(CONFIG_TINY_RCU) ? NULL : rcu_tasks_rude_scale_stats,
	.name		= "tasks-rude"
};

#define TASKS_RUDE_OPS &tasks_rude_ops,

#else // #ifdef CONFIG_TASKS_RUDE_RCU

#define TASKS_RUDE_OPS

#endif // #else // #ifdef CONFIG_TASKS_RUDE_RCU

#ifdef CONFIG_TASKS_TRACE_RCU

/*
 * Definitions for RCU-tasks-trace scalability testing.
 */

static int tasks_trace_scale_read_lock(void)
{
	rcu_read_lock_trace();
	return 0;
}

static void tasks_trace_scale_read_unlock(int idx)
{
	rcu_read_unlock_trace();
}

static void rcu_tasks_trace_scale_stats(void)
{
	rcu_tasks_trace_torture_stats_print(scale_type, SCALE_FLAG);
}

static struct rcu_scale_ops tasks_tracing_ops = {
	.ptype		= RCU_TASKS_FLAVOR,
	.init		= rcu_sync_scale_init,
	.readlock	= tasks_trace_scale_read_lock,
	.readunlock	= tasks_trace_scale_read_unlock,
	.get_gp_seq	= rcu_no_completed,
	.gp_diff	= rcu_seq_diff,
	.async		= call_rcu_tasks_trace,
	.gp_barrier	= rcu_barrier_tasks_trace,
	.sync		= synchronize_rcu_tasks_trace,
	.exp_sync	= synchronize_rcu_tasks_trace,
	.rso_gp_kthread	= get_rcu_tasks_trace_gp_kthread,
	.stats		= IS_ENABLED(CONFIG_TINY_RCU) ? NULL : rcu_tasks_trace_scale_stats,
	.name		= "tasks-tracing"
};

#define TASKS_TRACING_OPS &tasks_tracing_ops,

#else // #ifdef CONFIG_TASKS_TRACE_RCU

#define TASKS_TRACING_OPS

#endif // #else // #ifdef CONFIG_TASKS_TRACE_RCU

static unsigned long rcuscale_seq_diff(unsigned long new, unsigned long old)
{
	if (!cur_ops->gp_diff)
		return new - old;
	return cur_ops->gp_diff(new, old);
}

/*
 * If scalability tests complete, wait for shutdown to commence.
 */
static void rcu_scale_wait_shutdown(void)
{
	cond_resched_tasks_rcu_qs();
	if (atomic_read(&n_rcu_scale_writer_finished) < nrealwriters)
		return;
	while (!torture_must_stop())
		schedule_timeout_uninterruptible(1);
}

/*
 * RCU scalability reader kthread.  Repeatedly does empty RCU read-side
 * critical section, minimizing update-side interference.  However, the
 * point of this test is not to evaluate reader scalability, but instead
 * to serve as a test load for update-side scalability testing.
 */
static int
rcu_scale_reader(void *arg)
{
	unsigned long flags;
	int idx;
	long me = (long)arg;

	VERBOSE_SCALEOUT_STRING("rcu_scale_reader task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	set_user_nice(current, MAX_NICE);
	atomic_inc(&n_rcu_scale_reader_started);

	do {
		local_irq_save(flags);
		idx = cur_ops->readlock();
		cur_ops->readunlock(idx);
		local_irq_restore(flags);
		rcu_scale_wait_shutdown();
	} while (!torture_must_stop());
	torture_kthread_stopping("rcu_scale_reader");
	return 0;
}

/*
 * Allocate a writer_mblock structure for the specified rcu_scale_writer
 * task.
 */
static struct writer_mblock *rcu_scale_alloc(long me)
{
	struct llist_node *llnp;
	struct writer_freelist *wflp;
	struct writer_mblock *wmbp;

	if (WARN_ON_ONCE(!writer_freelists))
		return NULL;
	wflp = &writer_freelists[me];
	if (llist_empty(&wflp->ws_lhp)) {
		// ->ws_lhp is private to its rcu_scale_writer task.
		wmbp = container_of(llist_del_all(&wflp->ws_lhg), struct writer_mblock, wmb_node);
		wflp->ws_lhp.first = &wmbp->wmb_node;
	}
	llnp = llist_del_first(&wflp->ws_lhp);
	if (!llnp)
		return NULL;
	return container_of(llnp, struct writer_mblock, wmb_node);
}

/*
 * Free a writer_mblock structure to its rcu_scale_writer task.
 */
static void rcu_scale_free(struct writer_mblock *wmbp)
{
	struct writer_freelist *wflp;

	if (!wmbp)
		return;
	wflp = wmbp->wmb_wfl;
	llist_add(&wmbp->wmb_node, &wflp->ws_lhg);
}

/*
 * Callback function for asynchronous grace periods from rcu_scale_writer().
 */
static void rcu_scale_async_cb(struct rcu_head *rhp)
{
	struct writer_mblock *wmbp = container_of(rhp, struct writer_mblock, wmb_rh);
	struct writer_freelist *wflp = wmbp->wmb_wfl;

	atomic_dec(&wflp->ws_inflight);
	rcu_scale_free(wmbp);
}

/*
 * RCU scale writer kthread.  Repeatedly does a grace period.
 */
static int
rcu_scale_writer(void *arg)
{
	int i = 0;
	int i_max;
	unsigned long jdone;
	long me = (long)arg;
	bool selfreport = false;
	bool started = false, done = false, alldone = false;
	u64 t;
	DEFINE_TORTURE_RANDOM(tr);
	u64 *wdp;
	u64 *wdpp = writer_durations[me];
	struct writer_freelist *wflp = &writer_freelists[me];
	struct writer_mblock *wmbp = NULL;

	VERBOSE_SCALEOUT_STRING("rcu_scale_writer task started");
	WARN_ON(!wdpp);
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	current->flags |= PF_NO_SETAFFINITY;
	sched_set_fifo_low(current);

	if (holdoff)
		schedule_timeout_idle(holdoff * HZ);

	/*
	 * Wait until rcu_end_inkernel_boot() is called for normal GP tests
	 * so that RCU is not always expedited for normal GP tests.
	 * The system_state test is approximate, but works well in practice.
	 */
	while (!gp_exp && system_state != SYSTEM_RUNNING)
		schedule_timeout_uninterruptible(1);

	t = ktime_get_mono_fast_ns();
	if (atomic_inc_return(&n_rcu_scale_writer_started) >= nrealwriters) {
		t_rcu_scale_writer_started = t;
		if (gp_exp) {
			b_rcu_gp_test_started =
				cur_ops->exp_completed() / 2;
		} else {
			b_rcu_gp_test_started = cur_ops->get_gp_seq();
		}
	}

	jdone = jiffies + minruntime * HZ;
	do {
		bool gp_succeeded = false;

		if (writer_holdoff)
			udelay(writer_holdoff);
		if (writer_holdoff_jiffies)
			schedule_timeout_idle(torture_random(&tr) % writer_holdoff_jiffies + 1);
		wdp = &wdpp[i];
		*wdp = ktime_get_mono_fast_ns();
		if (gp_async && !WARN_ON_ONCE(!cur_ops->async)) {
			if (!wmbp)
				wmbp = rcu_scale_alloc(me);
			if (wmbp && atomic_read(&wflp->ws_inflight) < gp_async_max) {
				atomic_inc(&wflp->ws_inflight);
				cur_ops->async(&wmbp->wmb_rh, rcu_scale_async_cb);
				wmbp = NULL;
				gp_succeeded = true;
			} else if (!kthread_should_stop()) {
				cur_ops->gp_barrier();
			} else {
				rcu_scale_free(wmbp); /* Because we are stopping. */
				wmbp = NULL;
			}
		} else if (gp_exp) {
			cur_ops->exp_sync();
			gp_succeeded = true;
		} else {
			cur_ops->sync();
			gp_succeeded = true;
		}
		t = ktime_get_mono_fast_ns();
		*wdp = t - *wdp;
		i_max = i;
		if (!started &&
		    atomic_read(&n_rcu_scale_writer_started) >= nrealwriters)
			started = true;
		if (!done && i >= MIN_MEAS && time_after(jiffies, jdone)) {
			done = true;
			WRITE_ONCE(writer_done[me], true);
			sched_set_normal(current, 0);
			pr_alert("%s%s rcu_scale_writer %ld has %d measurements\n",
				 scale_type, SCALE_FLAG, me, MIN_MEAS);
			if (atomic_inc_return(&n_rcu_scale_writer_finished) >=
			    nrealwriters) {
				schedule_timeout_interruptible(10);
				rcu_ftrace_dump(DUMP_ALL);
				SCALEOUT_STRING("Test complete");
				t_rcu_scale_writer_finished = t;
				if (gp_exp) {
					b_rcu_gp_test_finished =
						cur_ops->exp_completed() / 2;
				} else {
					b_rcu_gp_test_finished =
						cur_ops->get_gp_seq();
				}
				if (shutdown) {
					smp_mb(); /* Assign before wake. */
					wake_up(&shutdown_wq);
				}
			}
		}
		if (done && !alldone &&
		    atomic_read(&n_rcu_scale_writer_finished) >= nrealwriters)
			alldone = true;
		if (done && !alldone && time_after(jiffies, jdone + HZ * 60)) {
			static atomic_t dumped;
			int i;

			if (!atomic_xchg(&dumped, 1)) {
				for (i = 0; i < nrealwriters; i++) {
					if (writer_done[i])
						continue;
					pr_info("%s: Task %ld flags writer %d:\n", __func__, me, i);
					sched_show_task(writer_tasks[i]);
				}
				if (cur_ops->stats)
					cur_ops->stats();
			}
		}
		if (!selfreport && time_after(jiffies, jdone + HZ * (70 + me))) {
			pr_info("%s: Writer %ld self-report: started %d done %d/%d->%d i %d jdone %lu.\n",
				__func__, me, started, done, writer_done[me], atomic_read(&n_rcu_scale_writer_finished), i, jiffies - jdone);
			selfreport = true;
		}
		if (gp_succeeded && started && !alldone && i < MAX_MEAS - 1)
			i++;
		rcu_scale_wait_shutdown();
	} while (!torture_must_stop());
	if (gp_async && cur_ops->async) {
		rcu_scale_free(wmbp);
		cur_ops->gp_barrier();
	}
	writer_n_durations[me] = i_max + 1;
	torture_kthread_stopping("rcu_scale_writer");
	return 0;
}

static void
rcu_scale_print_module_parms(struct rcu_scale_ops *cur_ops, const char *tag)
{
	pr_alert("%s" SCALE_FLAG
		 "--- %s: gp_async=%d gp_async_max=%d gp_exp=%d holdoff=%d minruntime=%d nreaders=%d nwriters=%d writer_holdoff=%d writer_holdoff_jiffies=%d verbose=%d shutdown=%d\n",
		 scale_type, tag, gp_async, gp_async_max, gp_exp, holdoff, minruntime, nrealreaders, nrealwriters, writer_holdoff, writer_holdoff_jiffies, verbose, shutdown);
}

/*
 * Return the number if non-negative.  If -1, the number of CPUs.
 * If less than -1, that much less than the number of CPUs, but
 * at least one.
 */
static int compute_real(int n)
{
	int nr;

	if (n >= 0) {
		nr = n;
	} else {
		nr = num_online_cpus() + 1 + n;
		if (nr <= 0)
			nr = 1;
	}
	return nr;
}

/*
 * kfree_rcu() scalability tests: Start a kfree_rcu() loop on all CPUs for number
 * of iterations and measure total time and number of GP for all iterations to complete.
 */

torture_param(int, kfree_nthreads, -1, "Number of threads running loops of kfree_rcu().");
torture_param(int, kfree_alloc_num, 8000, "Number of allocations and frees done in an iteration.");
torture_param(int, kfree_loops, 10, "Number of loops doing kfree_alloc_num allocations and frees.");
torture_param(bool, kfree_rcu_test_double, false, "Do we run a kfree_rcu() double-argument scale test?");
torture_param(bool, kfree_rcu_test_single, false, "Do we run a kfree_rcu() single-argument scale test?");

static struct task_struct **kfree_reader_tasks;
static int kfree_nrealthreads;
static atomic_t n_kfree_scale_thread_started;
static atomic_t n_kfree_scale_thread_ended;
static struct task_struct *kthread_tp;
static u64 kthread_stime;

struct kfree_obj {
	char kfree_obj[8];
	struct rcu_head rh;
};

/* Used if doing RCU-kfree'ing via call_rcu(). */
static void kfree_call_rcu(struct rcu_head *rh)
{
	struct kfree_obj *obj = container_of(rh, struct kfree_obj, rh);

	kfree(obj);
}

static int
kfree_scale_thread(void *arg)
{
	int i, loop = 0;
	long me = (long)arg;
	struct kfree_obj *alloc_ptr;
	u64 start_time, end_time;
	long long mem_begin, mem_during = 0;
	bool kfree_rcu_test_both;
	DEFINE_TORTURE_RANDOM(tr);

	VERBOSE_SCALEOUT_STRING("kfree_scale_thread task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	set_user_nice(current, MAX_NICE);
	kfree_rcu_test_both = (kfree_rcu_test_single == kfree_rcu_test_double);

	start_time = ktime_get_mono_fast_ns();

	if (atomic_inc_return(&n_kfree_scale_thread_started) >= kfree_nrealthreads) {
		if (gp_exp)
			b_rcu_gp_test_started = cur_ops->exp_completed() / 2;
		else
			b_rcu_gp_test_started = cur_ops->get_gp_seq();
	}

	do {
		if (!mem_during) {
			mem_during = mem_begin = si_mem_available();
		} else if (loop % (kfree_loops / 4) == 0) {
			mem_during = (mem_during + si_mem_available()) / 2;
		}

		for (i = 0; i < kfree_alloc_num; i++) {
			alloc_ptr = kcalloc(kfree_mult, sizeof(struct kfree_obj), GFP_KERNEL);
			if (!alloc_ptr)
				return -ENOMEM;

			if (kfree_by_call_rcu) {
				call_rcu(&(alloc_ptr->rh), kfree_call_rcu);
				continue;
			}

			// By default kfree_rcu_test_single and kfree_rcu_test_double are
			// initialized to false. If both have the same value (false or true)
			// both are randomly tested, otherwise only the one with value true
			// is tested.
			if ((kfree_rcu_test_single && !kfree_rcu_test_double) ||
					(kfree_rcu_test_both && torture_random(&tr) & 0x800))
				kfree_rcu_mightsleep(alloc_ptr);
			else
				kfree_rcu(alloc_ptr, rh);
		}

		cond_resched();
	} while (!torture_must_stop() && ++loop < kfree_loops);

	if (atomic_inc_return(&n_kfree_scale_thread_ended) >= kfree_nrealthreads) {
		end_time = ktime_get_mono_fast_ns();

		if (gp_exp)
			b_rcu_gp_test_finished = cur_ops->exp_completed() / 2;
		else
			b_rcu_gp_test_finished = cur_ops->get_gp_seq();

		pr_alert("Total time taken by all kfree'ers: %llu ns, loops: %d, batches: %ld, memory footprint: %lldMB\n",
		       (unsigned long long)(end_time - start_time), kfree_loops,
		       rcuscale_seq_diff(b_rcu_gp_test_finished, b_rcu_gp_test_started),
		       (mem_begin - mem_during) >> (20 - PAGE_SHIFT));

		if (shutdown) {
			smp_mb(); /* Assign before wake. */
			wake_up(&shutdown_wq);
		}
	}

	torture_kthread_stopping("kfree_scale_thread");
	return 0;
}

static void
kfree_scale_cleanup(void)
{
	int i;

	if (torture_cleanup_begin())
		return;

	if (kfree_reader_tasks) {
		for (i = 0; i < kfree_nrealthreads; i++)
			torture_stop_kthread(kfree_scale_thread,
					     kfree_reader_tasks[i]);
		kfree(kfree_reader_tasks);
		kfree_reader_tasks = NULL;
	}

	torture_cleanup_end();
}

/*
 * shutdown kthread.  Just waits to be awakened, then shuts down system.
 */
static int
kfree_scale_shutdown(void *arg)
{
	wait_event_idle(shutdown_wq,
			atomic_read(&n_kfree_scale_thread_ended) >= kfree_nrealthreads);

	smp_mb(); /* Wake before output. */

	kfree_scale_cleanup();
	kernel_power_off();
	return -EINVAL;
}

// Used if doing RCU-kfree'ing via call_rcu().
static unsigned long jiffies_at_lazy_cb;
static struct rcu_head lazy_test1_rh;
static int rcu_lazy_test1_cb_called;
static void call_rcu_lazy_test1(struct rcu_head *rh)
{
	jiffies_at_lazy_cb = jiffies;
	WRITE_ONCE(rcu_lazy_test1_cb_called, 1);
}

static int __init
kfree_scale_init(void)
{
	int firsterr = 0;
	long i;
	unsigned long jif_start;
	unsigned long orig_jif;

	pr_alert("%s" SCALE_FLAG
		 "--- kfree_rcu_test: kfree_mult=%d kfree_by_call_rcu=%d kfree_nthreads=%d kfree_alloc_num=%d kfree_loops=%d kfree_rcu_test_double=%d kfree_rcu_test_single=%d\n",
		 scale_type, kfree_mult, kfree_by_call_rcu, kfree_nthreads, kfree_alloc_num, kfree_loops, kfree_rcu_test_double, kfree_rcu_test_single);

	// Also, do a quick self-test to ensure laziness is as much as
	// expected.
	if (kfree_by_call_rcu && !IS_ENABLED(CONFIG_RCU_LAZY)) {
		pr_alert("CONFIG_RCU_LAZY is disabled, falling back to kfree_rcu() for delayed RCU kfree'ing\n");
		kfree_by_call_rcu = 0;
	}

	if (kfree_by_call_rcu) {
		/* do a test to check the timeout. */
		orig_jif = rcu_get_jiffies_lazy_flush();

		rcu_set_jiffies_lazy_flush(2 * HZ);
		rcu_barrier();

		jif_start = jiffies;
		jiffies_at_lazy_cb = 0;
		call_rcu(&lazy_test1_rh, call_rcu_lazy_test1);

		smp_cond_load_relaxed(&rcu_lazy_test1_cb_called, VAL == 1);

		rcu_set_jiffies_lazy_flush(orig_jif);

		if (WARN_ON_ONCE(jiffies_at_lazy_cb - jif_start < 2 * HZ)) {
			pr_alert("ERROR: call_rcu() CBs are not being lazy as expected!\n");
			firsterr = -1;
			goto unwind;
		}

		if (WARN_ON_ONCE(jiffies_at_lazy_cb - jif_start > 3 * HZ)) {
			pr_alert("ERROR: call_rcu() CBs are being too lazy!\n");
			firsterr = -1;
			goto unwind;
		}
	}

	kfree_nrealthreads = compute_real(kfree_nthreads);
	/* Start up the kthreads. */
	if (shutdown) {
		init_waitqueue_head(&shutdown_wq);
		firsterr = torture_create_kthread(kfree_scale_shutdown, NULL,
						  shutdown_task);
		if (torture_init_error(firsterr))
			goto unwind;
		schedule_timeout_uninterruptible(1);
	}

	pr_alert("kfree object size=%zu, kfree_by_call_rcu=%d\n",
			kfree_mult * sizeof(struct kfree_obj),
			kfree_by_call_rcu);

	kfree_reader_tasks = kcalloc(kfree_nrealthreads, sizeof(kfree_reader_tasks[0]),
			       GFP_KERNEL);
	if (kfree_reader_tasks == NULL) {
		firsterr = -ENOMEM;
		goto unwind;
	}

	for (i = 0; i < kfree_nrealthreads; i++) {
		firsterr = torture_create_kthread(kfree_scale_thread, (void *)i,
						  kfree_reader_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}

	while (atomic_read(&n_kfree_scale_thread_started) < kfree_nrealthreads)
		schedule_timeout_uninterruptible(1);

	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	kfree_scale_cleanup();
	return firsterr;
}

static void
rcu_scale_cleanup(void)
{
	int i;
	int j;
	int ngps = 0;
	u64 *wdp;
	u64 *wdpp;

	/*
	 * Would like warning at start, but everything is expedited
	 * during the mid-boot phase, so have to wait till the end.
	 */
	if (rcu_gp_is_expedited() && !rcu_gp_is_normal() && !gp_exp)
		SCALEOUT_ERRSTRING("All grace periods expedited, no normal ones to measure!");
	if (rcu_gp_is_normal() && gp_exp)
		SCALEOUT_ERRSTRING("All grace periods normal, no expedited ones to measure!");
	if (gp_exp && gp_async)
		SCALEOUT_ERRSTRING("No expedited async GPs, so went with async!");

	// If built-in, just report all of the GP kthread's CPU time.
	if (IS_BUILTIN(CONFIG_RCU_SCALE_TEST) && !kthread_tp && cur_ops->rso_gp_kthread)
		kthread_tp = cur_ops->rso_gp_kthread();
	if (kthread_tp) {
		u32 ns;
		u64 us;

		kthread_stime = kthread_tp->stime - kthread_stime;
		us = div_u64_rem(kthread_stime, 1000, &ns);
		pr_info("rcu_scale: Grace-period kthread CPU time: %llu.%03u us\n", us, ns);
		show_rcu_gp_kthreads();
	}
	if (kfree_rcu_test) {
		kfree_scale_cleanup();
		return;
	}

	if (torture_cleanup_begin())
		return;
	if (!cur_ops) {
		torture_cleanup_end();
		return;
	}

	if (reader_tasks) {
		for (i = 0; i < nrealreaders; i++)
			torture_stop_kthread(rcu_scale_reader,
					     reader_tasks[i]);
		kfree(reader_tasks);
		reader_tasks = NULL;
	}

	if (writer_tasks) {
		for (i = 0; i < nrealwriters; i++) {
			torture_stop_kthread(rcu_scale_writer,
					     writer_tasks[i]);
			if (!writer_n_durations)
				continue;
			j = writer_n_durations[i];
			pr_alert("%s%s writer %d gps: %d\n",
				 scale_type, SCALE_FLAG, i, j);
			ngps += j;
		}
		pr_alert("%s%s start: %llu end: %llu duration: %llu gps: %d batches: %ld\n",
			 scale_type, SCALE_FLAG,
			 t_rcu_scale_writer_started, t_rcu_scale_writer_finished,
			 t_rcu_scale_writer_finished -
			 t_rcu_scale_writer_started,
			 ngps,
			 rcuscale_seq_diff(b_rcu_gp_test_finished,
					   b_rcu_gp_test_started));
		for (i = 0; i < nrealwriters; i++) {
			if (!writer_durations)
				break;
			if (!writer_n_durations)
				continue;
			wdpp = writer_durations[i];
			if (!wdpp)
				continue;
			for (j = 0; j < writer_n_durations[i]; j++) {
				wdp = &wdpp[j];
				pr_alert("%s%s %4d writer-duration: %5d %llu\n",
					scale_type, SCALE_FLAG,
					i, j, *wdp);
				if (j % 100 == 0)
					schedule_timeout_uninterruptible(1);
			}
			kfree(writer_durations[i]);
			if (writer_freelists) {
				int ctr = 0;
				struct llist_node *llnp;
				struct writer_freelist *wflp = &writer_freelists[i];

				if (wflp->ws_mblocks) {
					llist_for_each(llnp, wflp->ws_lhg.first)
						ctr++;
					llist_for_each(llnp, wflp->ws_lhp.first)
						ctr++;
					WARN_ONCE(ctr != gp_async_max,
						  "%s: ctr = %d gp_async_max = %d\n",
						  __func__, ctr, gp_async_max);
					kfree(wflp->ws_mblocks);
				}
			}
		}
		kfree(writer_tasks);
		writer_tasks = NULL;
		kfree(writer_durations);
		writer_durations = NULL;
		kfree(writer_n_durations);
		writer_n_durations = NULL;
		kfree(writer_done);
		writer_done = NULL;
		kfree(writer_freelists);
		writer_freelists = NULL;
	}

	/* Do torture-type-specific cleanup operations.  */
	if (cur_ops->cleanup != NULL)
		cur_ops->cleanup();

	torture_cleanup_end();
}

/*
 * RCU scalability shutdown kthread.  Just waits to be awakened, then shuts
 * down system.
 */
static int
rcu_scale_shutdown(void *arg)
{
	wait_event_idle(shutdown_wq, atomic_read(&n_rcu_scale_writer_finished) >= nrealwriters);
	smp_mb(); /* Wake before output. */
	rcu_scale_cleanup();
	kernel_power_off();
	return -EINVAL;
}

static int __init
rcu_scale_init(void)
{
	int firsterr = 0;
	long i;
	long j;
	static struct rcu_scale_ops *scale_ops[] = {
		&rcu_ops, &srcu_ops, &srcud_ops, TASKS_OPS TASKS_RUDE_OPS TASKS_TRACING_OPS
	};

	if (!torture_init_begin(scale_type, verbose))
		return -EBUSY;

	/* Process args and announce that the scalability'er is on the job. */
	for (i = 0; i < ARRAY_SIZE(scale_ops); i++) {
		cur_ops = scale_ops[i];
		if (strcmp(scale_type, cur_ops->name) == 0)
			break;
	}
	if (i == ARRAY_SIZE(scale_ops)) {
		pr_alert("rcu-scale: invalid scale type: \"%s\"\n", scale_type);
		pr_alert("rcu-scale types:");
		for (i = 0; i < ARRAY_SIZE(scale_ops); i++)
			pr_cont(" %s", scale_ops[i]->name);
		pr_cont("\n");
		firsterr = -EINVAL;
		cur_ops = NULL;
		goto unwind;
	}
	if (cur_ops->init)
		cur_ops->init();

	if (cur_ops->rso_gp_kthread) {
		kthread_tp = cur_ops->rso_gp_kthread();
		if (kthread_tp)
			kthread_stime = kthread_tp->stime;
	}
	if (kfree_rcu_test)
		return kfree_scale_init();

	nrealwriters = compute_real(nwriters);
	nrealreaders = compute_real(nreaders);
	atomic_set(&n_rcu_scale_reader_started, 0);
	atomic_set(&n_rcu_scale_writer_started, 0);
	atomic_set(&n_rcu_scale_writer_finished, 0);
	rcu_scale_print_module_parms(cur_ops, "Start of test");

	/* Start up the kthreads. */

	if (shutdown) {
		init_waitqueue_head(&shutdown_wq);
		firsterr = torture_create_kthread(rcu_scale_shutdown, NULL,
						  shutdown_task);
		if (torture_init_error(firsterr))
			goto unwind;
		schedule_timeout_uninterruptible(1);
	}
	reader_tasks = kcalloc(nrealreaders, sizeof(reader_tasks[0]),
			       GFP_KERNEL);
	if (reader_tasks == NULL) {
		SCALEOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealreaders; i++) {
		firsterr = torture_create_kthread(rcu_scale_reader, (void *)i,
						  reader_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	while (atomic_read(&n_rcu_scale_reader_started) < nrealreaders)
		schedule_timeout_uninterruptible(1);
	writer_tasks = kcalloc(nrealwriters, sizeof(writer_tasks[0]), GFP_KERNEL);
	writer_durations = kcalloc(nrealwriters, sizeof(*writer_durations), GFP_KERNEL);
	writer_n_durations = kcalloc(nrealwriters, sizeof(*writer_n_durations), GFP_KERNEL);
	writer_done = kcalloc(nrealwriters, sizeof(writer_done[0]), GFP_KERNEL);
	if (gp_async) {
		if (gp_async_max <= 0) {
			pr_warn("%s: gp_async_max = %d must be greater than zero.\n",
				__func__, gp_async_max);
			WARN_ON_ONCE(IS_BUILTIN(CONFIG_RCU_TORTURE_TEST));
			firsterr = -EINVAL;
			goto unwind;
		}
		writer_freelists = kcalloc(nrealwriters, sizeof(writer_freelists[0]), GFP_KERNEL);
	}
	if (!writer_tasks || !writer_durations || !writer_n_durations || !writer_done ||
	    (gp_async && !writer_freelists)) {
		SCALEOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealwriters; i++) {
		writer_durations[i] =
			kcalloc(MAX_MEAS, sizeof(*writer_durations[i]),
				GFP_KERNEL);
		if (!writer_durations[i]) {
			firsterr = -ENOMEM;
			goto unwind;
		}
		if (writer_freelists) {
			struct writer_freelist *wflp = &writer_freelists[i];

			init_llist_head(&wflp->ws_lhg);
			init_llist_head(&wflp->ws_lhp);
			wflp->ws_mblocks = kcalloc(gp_async_max, sizeof(wflp->ws_mblocks[0]),
						   GFP_KERNEL);
			if (!wflp->ws_mblocks) {
				firsterr = -ENOMEM;
				goto unwind;
			}
			for (j = 0; j < gp_async_max; j++) {
				struct writer_mblock *wmbp = &wflp->ws_mblocks[j];

				wmbp->wmb_wfl = wflp;
				llist_add(&wmbp->wmb_node, &wflp->ws_lhp);
			}
		}
		firsterr = torture_create_kthread(rcu_scale_writer, (void *)i,
						  writer_tasks[i]);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	rcu_scale_cleanup();
	if (shutdown) {
		WARN_ON(!IS_MODULE(CONFIG_RCU_SCALE_TEST));
		kernel_power_off();
	}
	return firsterr;
}

module_init(rcu_scale_init);
module_exit(rcu_scale_cleanup);
