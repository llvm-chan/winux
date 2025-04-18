// SPDX-License-Identifier: GPL-2.0+
//
// Torture test for smp_call_function() and friends.
//
// Copyright (C) Facebook, 2020.
//
// Author: Paul E. McKenney <paulmck@kernel.org>

#define pr_fmt(fmt) fmt

#include <winux/atomic.h>
#include <winux/bitops.h>
#include <winux/completion.h>
#include <winux/cpu.h>
#include <winux/delay.h>
#include <winux/err.h>
#include <winux/init.h>
#include <winux/interrupt.h>
#include <winux/kthread.h>
#include <winux/kernel.h>
#include <winux/mm.h>
#include <winux/module.h>
#include <winux/moduleparam.h>
#include <winux/notifier.h>
#include <winux/percpu.h>
#include <winux/rcupdate.h>
#include <winux/rcupdate_trace.h>
#include <winux/reboot.h>
#include <winux/sched.h>
#include <winux/spinlock.h>
#include <winux/smp.h>
#include <winux/stat.h>
#include <winux/srcu.h>
#include <winux/slab.h>
#include <winux/torture.h>
#include <winux/types.h>

#define SCFTORT_STRING "scftorture"
#define SCFTORT_FLAG SCFTORT_STRING ": "

#define VERBOSE_SCFTORTOUT(s, x...) \
	do { if (verbose) pr_alert(SCFTORT_FLAG s "\n", ## x); } while (0)

#define SCFTORTOUT_ERRSTRING(s, x...) pr_alert(SCFTORT_FLAG "!!! " s "\n", ## x)

MODULE_DESCRIPTION("Torture tests on the smp_call_function() family of primitives");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paul E. McKenney <paulmck@kernel.org>");

// Wait until there are multiple CPUs before starting test.
torture_param(int, holdoff, IS_BUILTIN(CONFIG_SCF_TORTURE_TEST) ? 10 : 0,
	      "Holdoff time before test start (s)");
torture_param(int, longwait, 0, "Include ridiculously long waits? (seconds)");
torture_param(int, nthreads, -1, "# threads, defaults to -1 for all CPUs.");
torture_param(int, onoff_holdoff, 0, "Time after boot before CPU hotplugs (s)");
torture_param(int, onoff_interval, 0, "Time between CPU hotplugs (s), 0=disable");
torture_param(int, shutdown_secs, 0, "Shutdown time (ms), <= zero to disable.");
torture_param(int, stat_interval, 60, "Number of seconds between stats printk()s.");
torture_param(int, stutter, 5, "Number of jiffies to run/halt test, 0=disable");
torture_param(bool, use_cpus_read_lock, 0, "Use cpus_read_lock() to exclude CPU hotplug.");
torture_param(int, verbose, 0, "Enable verbose debugging printk()s");
torture_param(int, weight_resched, -1, "Testing weight for resched_cpu() operations.");
torture_param(int, weight_single, -1, "Testing weight for single-CPU no-wait operations.");
torture_param(int, weight_single_rpc, -1, "Testing weight for single-CPU RPC operations.");
torture_param(int, weight_single_wait, -1, "Testing weight for single-CPU operations.");
torture_param(int, weight_many, -1, "Testing weight for multi-CPU no-wait operations.");
torture_param(int, weight_many_wait, -1, "Testing weight for multi-CPU operations.");
torture_param(int, weight_all, -1, "Testing weight for all-CPU no-wait operations.");
torture_param(int, weight_all_wait, -1, "Testing weight for all-CPU operations.");

static char *torture_type = "";

#ifdef MODULE
# define SCFTORT_SHUTDOWN 0
#else
# define SCFTORT_SHUTDOWN 1
#endif

torture_param(bool, shutdown, SCFTORT_SHUTDOWN, "Shutdown at end of torture test.");

struct scf_statistics {
	struct task_struct *task;
	int cpu;
	long long n_resched;
	long long n_single;
	long long n_single_ofl;
	long long n_single_rpc;
	long long n_single_rpc_ofl;
	long long n_single_wait;
	long long n_single_wait_ofl;
	long long n_many;
	long long n_many_wait;
	long long n_all;
	long long n_all_wait;
};

static struct scf_statistics *scf_stats_p;
static struct task_struct *scf_torture_stats_task;
static DEFINE_PER_CPU(long long, scf_invoked_count);
static DEFINE_PER_CPU(struct llist_head, scf_free_pool);

// Data for random primitive selection
#define SCF_PRIM_RESCHED	0
#define SCF_PRIM_SINGLE		1
#define SCF_PRIM_SINGLE_RPC	2
#define SCF_PRIM_MANY		3
#define SCF_PRIM_ALL		4
#define SCF_NPRIMS		8 // Need wait and no-wait versions of each,
				  //  except for SCF_PRIM_RESCHED and
				  //  SCF_PRIM_SINGLE_RPC.

static char *scf_prim_name[] = {
	"resched_cpu",
	"smp_call_function_single",
	"smp_call_function_single_rpc",
	"smp_call_function_many",
	"smp_call_function",
};

struct scf_selector {
	unsigned long scfs_weight;
	int scfs_prim;
	bool scfs_wait;
};
static struct scf_selector scf_sel_array[SCF_NPRIMS];
static int scf_sel_array_len;
static unsigned long scf_sel_totweight;

// Communicate between caller and handler.
struct scf_check {
	bool scfc_in;
	bool scfc_out;
	int scfc_cpu; // -1 for not _single().
	bool scfc_wait;
	bool scfc_rpc;
	struct completion scfc_completion;
	struct llist_node scf_node;
};

// Use to wait for all threads to start.
static atomic_t n_started;
static atomic_t n_errs;
static atomic_t n_mb_in_errs;
static atomic_t n_mb_out_errs;
static atomic_t n_alloc_errs;
static bool scfdone;
static char *bangstr = "";

static DEFINE_TORTURE_RANDOM_PERCPU(scf_torture_rand);

extern void resched_cpu(int cpu); // An alternative IPI vector.

static void scf_add_to_free_list(struct scf_check *scfcp)
{
	struct llist_head *pool;
	unsigned int cpu;

	if (!scfcp)
		return;
	cpu = raw_smp_processor_id() % nthreads;
	pool = &per_cpu(scf_free_pool, cpu);
	llist_add(&scfcp->scf_node, pool);
}

static void scf_cleanup_free_list(unsigned int cpu)
{
	struct llist_head *pool;
	struct llist_node *node;
	struct scf_check *scfcp;

	pool = &per_cpu(scf_free_pool, cpu);
	node = llist_del_all(pool);
	while (node) {
		scfcp = llist_entry(node, struct scf_check, scf_node);
		node = node->next;
		kfree(scfcp);
	}
}

// Print torture statistics.  Caller must ensure serialization.
static void scf_torture_stats_print(void)
{
	int cpu;
	int i;
	long long invoked_count = 0;
	bool isdone = READ_ONCE(scfdone);
	struct scf_statistics scfs = {};

	for_each_possible_cpu(cpu)
		invoked_count += data_race(per_cpu(scf_invoked_count, cpu));
	for (i = 0; i < nthreads; i++) {
		scfs.n_resched += scf_stats_p[i].n_resched;
		scfs.n_single += scf_stats_p[i].n_single;
		scfs.n_single_ofl += scf_stats_p[i].n_single_ofl;
		scfs.n_single_rpc += scf_stats_p[i].n_single_rpc;
		scfs.n_single_wait += scf_stats_p[i].n_single_wait;
		scfs.n_single_wait_ofl += scf_stats_p[i].n_single_wait_ofl;
		scfs.n_many += scf_stats_p[i].n_many;
		scfs.n_many_wait += scf_stats_p[i].n_many_wait;
		scfs.n_all += scf_stats_p[i].n_all;
		scfs.n_all_wait += scf_stats_p[i].n_all_wait;
	}
	if (atomic_read(&n_errs) || atomic_read(&n_mb_in_errs) ||
	    atomic_read(&n_mb_out_errs) ||
	    (!IS_ENABLED(CONFIG_KASAN) && atomic_read(&n_alloc_errs)))
		bangstr = "!!! ";
	pr_alert("%s %sscf_invoked_count %s: %lld resched: %lld single: %lld/%lld single_ofl: %lld/%lld single_rpc: %lld single_rpc_ofl: %lld many: %lld/%lld all: %lld/%lld ",
		 SCFTORT_FLAG, bangstr, isdone ? "VER" : "ver", invoked_count, scfs.n_resched,
		 scfs.n_single, scfs.n_single_wait, scfs.n_single_ofl, scfs.n_single_wait_ofl,
		 scfs.n_single_rpc, scfs.n_single_rpc_ofl,
		 scfs.n_many, scfs.n_many_wait, scfs.n_all, scfs.n_all_wait);
	torture_onoff_stats();
	pr_cont("ste: %d stnmie: %d stnmoe: %d staf: %d\n", atomic_read(&n_errs),
		atomic_read(&n_mb_in_errs), atomic_read(&n_mb_out_errs),
		atomic_read(&n_alloc_errs));
}

// Periodically prints torture statistics, if periodic statistics printing
// was specified via the stat_interval module parameter.
static int
scf_torture_stats(void *arg)
{
	VERBOSE_TOROUT_STRING("scf_torture_stats task started");
	do {
		schedule_timeout_interruptible(stat_interval * HZ);
		scf_torture_stats_print();
		torture_shutdown_absorb("scf_torture_stats");
	} while (!torture_must_stop());
	torture_kthread_stopping("scf_torture_stats");
	return 0;
}

// Add a primitive to the scf_sel_array[].
static void scf_sel_add(unsigned long weight, int prim, bool wait)
{
	struct scf_selector *scfsp = &scf_sel_array[scf_sel_array_len];

	// If no weight, if array would overflow, if computing three-place
	// percentages would overflow, or if the scf_prim_name[] array would
	// overflow, don't bother.  In the last three two cases, complain.
	if (!weight ||
	    WARN_ON_ONCE(scf_sel_array_len >= ARRAY_SIZE(scf_sel_array)) ||
	    WARN_ON_ONCE(0 - 100000 * weight <= 100000 * scf_sel_totweight) ||
	    WARN_ON_ONCE(prim >= ARRAY_SIZE(scf_prim_name)))
		return;
	scf_sel_totweight += weight;
	scfsp->scfs_weight = scf_sel_totweight;
	scfsp->scfs_prim = prim;
	scfsp->scfs_wait = wait;
	scf_sel_array_len++;
}

// Dump out weighting percentages for scf_prim_name[] array.
static void scf_sel_dump(void)
{
	int i;
	unsigned long oldw = 0;
	struct scf_selector *scfsp;
	unsigned long w;

	for (i = 0; i < scf_sel_array_len; i++) {
		scfsp = &scf_sel_array[i];
		w = (scfsp->scfs_weight - oldw) * 100000 / scf_sel_totweight;
		pr_info("%s: %3lu.%03lu %s(%s)\n", __func__, w / 1000, w % 1000,
			scf_prim_name[scfsp->scfs_prim],
			scfsp->scfs_wait ? "wait" : "nowait");
		oldw = scfsp->scfs_weight;
	}
}

// Randomly pick a primitive and wait/nowait, based on weightings.
static struct scf_selector *scf_sel_rand(struct torture_random_state *trsp)
{
	int i;
	unsigned long w = torture_random(trsp) % (scf_sel_totweight + 1);

	for (i = 0; i < scf_sel_array_len; i++)
		if (scf_sel_array[i].scfs_weight >= w)
			return &scf_sel_array[i];
	WARN_ON_ONCE(1);
	return &scf_sel_array[0];
}

// Update statistics and occasionally burn up mass quantities of CPU time,
// if told to do so via scftorture.longwait.  Otherwise, occasionally burn
// a little bit.
static void scf_handler(void *scfc_in)
{
	int i;
	int j;
	unsigned long r = torture_random(this_cpu_ptr(&scf_torture_rand));
	struct scf_check *scfcp = scfc_in;

	if (likely(scfcp)) {
		WRITE_ONCE(scfcp->scfc_out, false); // For multiple receivers.
		if (WARN_ON_ONCE(unlikely(!READ_ONCE(scfcp->scfc_in))))
			atomic_inc(&n_mb_in_errs);
	}
	this_cpu_inc(scf_invoked_count);
	if (longwait <= 0) {
		if (!(r & 0xffc0)) {
			udelay(r & 0x3f);
			goto out;
		}
	}
	if (r & 0xfff)
		goto out;
	r = (r >> 12);
	if (longwait <= 0) {
		udelay((r & 0xff) + 1);
		goto out;
	}
	r = r % longwait + 1;
	for (i = 0; i < r; i++) {
		for (j = 0; j < 1000; j++) {
			udelay(1000);
			cpu_relax();
		}
	}
out:
	if (unlikely(!scfcp))
		return;
	if (scfcp->scfc_wait) {
		WRITE_ONCE(scfcp->scfc_out, true);
		if (scfcp->scfc_rpc)
			complete(&scfcp->scfc_completion);
	} else {
		scf_add_to_free_list(scfcp);
	}
}

// As above, but check for correct CPU.
static void scf_handler_1(void *scfc_in)
{
	struct scf_check *scfcp = scfc_in;

	if (likely(scfcp) && WARN_ONCE(smp_processor_id() != scfcp->scfc_cpu, "%s: Wanted CPU %d got CPU %d\n", __func__, scfcp->scfc_cpu, smp_processor_id())) {
		atomic_inc(&n_errs);
	}
	scf_handler(scfcp);
}

// Randomly do an smp_call_function*() invocation.
static void scftorture_invoke_one(struct scf_statistics *scfp, struct torture_random_state *trsp)
{
	bool allocfail = false;
	uintptr_t cpu;
	int ret = 0;
	struct scf_check *scfcp = NULL;
	struct scf_selector *scfsp = scf_sel_rand(trsp);

	if (scfsp->scfs_prim == SCF_PRIM_SINGLE || scfsp->scfs_wait) {
		scfcp = kmalloc(sizeof(*scfcp), GFP_ATOMIC);
		if (!scfcp) {
			WARN_ON_ONCE(!IS_ENABLED(CONFIG_KASAN));
			atomic_inc(&n_alloc_errs);
			allocfail = true;
		} else {
			scfcp->scfc_cpu = -1;
			scfcp->scfc_wait = scfsp->scfs_wait;
			scfcp->scfc_out = false;
			scfcp->scfc_rpc = false;
		}
	}
	if (use_cpus_read_lock)
		cpus_read_lock();
	else
		preempt_disable();
	switch (scfsp->scfs_prim) {
	case SCF_PRIM_RESCHED:
		if (IS_BUILTIN(CONFIG_SCF_TORTURE_TEST)) {
			cpu = torture_random(trsp) % nr_cpu_ids;
			scfp->n_resched++;
			resched_cpu(cpu);
			this_cpu_inc(scf_invoked_count);
		}
		break;
	case SCF_PRIM_SINGLE:
		cpu = torture_random(trsp) % nr_cpu_ids;
		if (scfsp->scfs_wait)
			scfp->n_single_wait++;
		else
			scfp->n_single++;
		if (scfcp) {
			scfcp->scfc_cpu = cpu;
			barrier(); // Prevent race-reduction compiler optimizations.
			scfcp->scfc_in = true;
		}
		ret = smp_call_function_single(cpu, scf_handler_1, (void *)scfcp, scfsp->scfs_wait);
		if (ret) {
			if (scfsp->scfs_wait)
				scfp->n_single_wait_ofl++;
			else
				scfp->n_single_ofl++;
			scf_add_to_free_list(scfcp);
			scfcp = NULL;
		}
		break;
	case SCF_PRIM_SINGLE_RPC:
		if (!scfcp)
			break;
		cpu = torture_random(trsp) % nr_cpu_ids;
		scfp->n_single_rpc++;
		scfcp->scfc_cpu = cpu;
		scfcp->scfc_wait = true;
		init_completion(&scfcp->scfc_completion);
		scfcp->scfc_rpc = true;
		barrier(); // Prevent race-reduction compiler optimizations.
		scfcp->scfc_in = true;
		ret = smp_call_function_single(cpu, scf_handler_1, (void *)scfcp, 0);
		if (!ret) {
			if (use_cpus_read_lock)
				cpus_read_unlock();
			else
				preempt_enable();
			wait_for_completion(&scfcp->scfc_completion);
			if (use_cpus_read_lock)
				cpus_read_lock();
			else
				preempt_disable();
		} else {
			scfp->n_single_rpc_ofl++;
			scf_add_to_free_list(scfcp);
			scfcp = NULL;
		}
		break;
	case SCF_PRIM_MANY:
		if (scfsp->scfs_wait)
			scfp->n_many_wait++;
		else
			scfp->n_many++;
		if (scfcp) {
			barrier(); // Prevent race-reduction compiler optimizations.
			scfcp->scfc_in = true;
		}
		smp_call_function_many(cpu_online_mask, scf_handler, scfcp, scfsp->scfs_wait);
		break;
	case SCF_PRIM_ALL:
		if (scfsp->scfs_wait)
			scfp->n_all_wait++;
		else
			scfp->n_all++;
		if (scfcp) {
			barrier(); // Prevent race-reduction compiler optimizations.
			scfcp->scfc_in = true;
		}
		smp_call_function(scf_handler, scfcp, scfsp->scfs_wait);
		break;
	default:
		WARN_ON_ONCE(1);
		if (scfcp)
			scfcp->scfc_out = true;
	}
	if (scfcp && scfsp->scfs_wait) {
		if (WARN_ON_ONCE((num_online_cpus() > 1 || scfsp->scfs_prim == SCF_PRIM_SINGLE) &&
				 !scfcp->scfc_out)) {
			pr_warn("%s: Memory-ordering failure, scfs_prim: %d.\n", __func__, scfsp->scfs_prim);
			atomic_inc(&n_mb_out_errs); // Leak rather than trash!
		} else {
			scf_add_to_free_list(scfcp);
		}
		barrier(); // Prevent race-reduction compiler optimizations.
	}
	if (use_cpus_read_lock)
		cpus_read_unlock();
	else
		preempt_enable();
	if (allocfail)
		schedule_timeout_idle((1 + longwait) * HZ);  // Let no-wait handlers complete.
	else if (!(torture_random(trsp) & 0xfff))
		schedule_timeout_uninterruptible(1);
}

// SCF test kthread.  Repeatedly does calls to members of the
// smp_call_function() family of functions.
static int scftorture_invoker(void *arg)
{
	int cpu;
	int curcpu;
	DEFINE_TORTURE_RANDOM(rand);
	struct scf_statistics *scfp = (struct scf_statistics *)arg;
	bool was_offline = false;

	VERBOSE_SCFTORTOUT("scftorture_invoker %d: task started", scfp->cpu);
	cpu = scfp->cpu % nr_cpu_ids;
	WARN_ON_ONCE(set_cpus_allowed_ptr(current, cpumask_of(cpu)));
	set_user_nice(current, MAX_NICE);
	if (holdoff)
		schedule_timeout_interruptible(holdoff * HZ);

	VERBOSE_SCFTORTOUT("scftorture_invoker %d: Waiting for all SCF torturers from cpu %d", scfp->cpu, raw_smp_processor_id());

	// Make sure that the CPU is affinitized appropriately during testing.
	curcpu = raw_smp_processor_id();
	WARN_ONCE(curcpu != cpu,
		  "%s: Wanted CPU %d, running on %d, nr_cpu_ids = %d\n",
		  __func__, scfp->cpu, curcpu, nr_cpu_ids);

	if (!atomic_dec_return(&n_started))
		while (atomic_read_acquire(&n_started)) {
			if (torture_must_stop()) {
				VERBOSE_SCFTORTOUT("scftorture_invoker %d ended before starting", scfp->cpu);
				goto end;
			}
			schedule_timeout_uninterruptible(1);
		}

	VERBOSE_SCFTORTOUT("scftorture_invoker %d started", scfp->cpu);

	do {
		scf_cleanup_free_list(cpu);

		scftorture_invoke_one(scfp, &rand);
		while (cpu_is_offline(cpu) && !torture_must_stop()) {
			schedule_timeout_interruptible(HZ / 5);
			was_offline = true;
		}
		if (was_offline) {
			set_cpus_allowed_ptr(current, cpumask_of(cpu));
			was_offline = false;
		}
		cond_resched();
		stutter_wait("scftorture_invoker");
	} while (!torture_must_stop());

	VERBOSE_SCFTORTOUT("scftorture_invoker %d ended", scfp->cpu);
end:
	torture_kthread_stopping("scftorture_invoker");
	return 0;
}

static void
scftorture_print_module_parms(const char *tag)
{
	pr_alert(SCFTORT_FLAG
		 "--- %s:  verbose=%d holdoff=%d longwait=%d nthreads=%d onoff_holdoff=%d onoff_interval=%d shutdown_secs=%d stat_interval=%d stutter=%d use_cpus_read_lock=%d, weight_resched=%d, weight_single=%d, weight_single_rpc=%d, weight_single_wait=%d, weight_many=%d, weight_many_wait=%d, weight_all=%d, weight_all_wait=%d\n", tag,
		 verbose, holdoff, longwait, nthreads, onoff_holdoff, onoff_interval, shutdown, stat_interval, stutter, use_cpus_read_lock, weight_resched, weight_single, weight_single_rpc, weight_single_wait, weight_many, weight_many_wait, weight_all, weight_all_wait);
}

static void scf_cleanup_handler(void *unused)
{
}

static void scf_torture_cleanup(void)
{
	int i;

	if (torture_cleanup_begin())
		return;

	WRITE_ONCE(scfdone, true);
	if (nthreads && scf_stats_p)
		for (i = 0; i < nthreads; i++)
			torture_stop_kthread("scftorture_invoker", scf_stats_p[i].task);
	else
		goto end;
	smp_call_function(scf_cleanup_handler, NULL, 1);
	torture_stop_kthread(scf_torture_stats, scf_torture_stats_task);
	scf_torture_stats_print();  // -After- the stats thread is stopped!
	kfree(scf_stats_p);  // -After- the last stats print has completed!
	scf_stats_p = NULL;

	for (i = 0; i < nr_cpu_ids; i++)
		scf_cleanup_free_list(i);

	if (atomic_read(&n_errs) || atomic_read(&n_mb_in_errs) || atomic_read(&n_mb_out_errs))
		scftorture_print_module_parms("End of test: FAILURE");
	else if (torture_onoff_failures())
		scftorture_print_module_parms("End of test: LOCK_HOTPLUG");
	else
		scftorture_print_module_parms("End of test: SUCCESS");

end:
	torture_cleanup_end();
}

static int __init scf_torture_init(void)
{
	long i;
	int firsterr = 0;
	unsigned long weight_resched1 = weight_resched;
	unsigned long weight_single1 = weight_single;
	unsigned long weight_single_rpc1 = weight_single_rpc;
	unsigned long weight_single_wait1 = weight_single_wait;
	unsigned long weight_many1 = weight_many;
	unsigned long weight_many_wait1 = weight_many_wait;
	unsigned long weight_all1 = weight_all;
	unsigned long weight_all_wait1 = weight_all_wait;

	if (!torture_init_begin(SCFTORT_STRING, verbose))
		return -EBUSY;

	scftorture_print_module_parms("Start of test");

	if (weight_resched <= 0 &&
	    weight_single <= 0 && weight_single_rpc <= 0 && weight_single_wait <= 0 &&
	    weight_many <= 0 && weight_many_wait <= 0 &&
	    weight_all <= 0 && weight_all_wait <= 0) {
		weight_resched1 = weight_resched == 0 ? 0 : 2 * nr_cpu_ids;
		weight_single1 = weight_single == 0 ? 0 : 2 * nr_cpu_ids;
		weight_single_rpc1 = weight_single_rpc == 0 ? 0 : 2 * nr_cpu_ids;
		weight_single_wait1 = weight_single_wait == 0 ? 0 : 2 * nr_cpu_ids;
		weight_many1 = weight_many == 0 ? 0 : 2;
		weight_many_wait1 = weight_many_wait == 0 ? 0 : 2;
		weight_all1 = weight_all == 0 ? 0 : 1;
		weight_all_wait1 = weight_all_wait == 0 ? 0 : 1;
	} else {
		if (weight_resched == -1)
			weight_resched1 = 0;
		if (weight_single == -1)
			weight_single1 = 0;
		if (weight_single_rpc == -1)
			weight_single_rpc1 = 0;
		if (weight_single_wait == -1)
			weight_single_wait1 = 0;
		if (weight_many == -1)
			weight_many1 = 0;
		if (weight_many_wait == -1)
			weight_many_wait1 = 0;
		if (weight_all == -1)
			weight_all1 = 0;
		if (weight_all_wait == -1)
			weight_all_wait1 = 0;
	}
	if (weight_resched1 == 0 && weight_single1 == 0 && weight_single_rpc1 == 0 &&
	    weight_single_wait1 == 0 && weight_many1 == 0 && weight_many_wait1 == 0 &&
	    weight_all1 == 0 && weight_all_wait1 == 0) {
		SCFTORTOUT_ERRSTRING("all zero weights makes no sense");
		firsterr = -EINVAL;
		goto unwind;
	}
	if (IS_BUILTIN(CONFIG_SCF_TORTURE_TEST))
		scf_sel_add(weight_resched1, SCF_PRIM_RESCHED, false);
	else if (weight_resched1)
		SCFTORTOUT_ERRSTRING("built as module, weight_resched ignored");
	scf_sel_add(weight_single1, SCF_PRIM_SINGLE, false);
	scf_sel_add(weight_single_rpc1, SCF_PRIM_SINGLE_RPC, true);
	scf_sel_add(weight_single_wait1, SCF_PRIM_SINGLE, true);
	scf_sel_add(weight_many1, SCF_PRIM_MANY, false);
	scf_sel_add(weight_many_wait1, SCF_PRIM_MANY, true);
	scf_sel_add(weight_all1, SCF_PRIM_ALL, false);
	scf_sel_add(weight_all_wait1, SCF_PRIM_ALL, true);
	scf_sel_dump();

	if (onoff_interval > 0) {
		firsterr = torture_onoff_init(onoff_holdoff * HZ, onoff_interval, NULL);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	if (shutdown_secs > 0) {
		firsterr = torture_shutdown_init(shutdown_secs, scf_torture_cleanup);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	if (stutter > 0) {
		firsterr = torture_stutter_init(stutter, stutter);
		if (torture_init_error(firsterr))
			goto unwind;
	}

	// Worker tasks invoking smp_call_function().
	if (nthreads < 0)
		nthreads = num_online_cpus();
	scf_stats_p = kcalloc(nthreads, sizeof(scf_stats_p[0]), GFP_KERNEL);
	if (!scf_stats_p) {
		SCFTORTOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}

	VERBOSE_SCFTORTOUT("Starting %d smp_call_function() threads", nthreads);

	atomic_set(&n_started, nthreads);
	for (i = 0; i < nthreads; i++) {
		scf_stats_p[i].cpu = i;
		firsterr = torture_create_kthread(scftorture_invoker, (void *)&scf_stats_p[i],
						  scf_stats_p[i].task);
		if (torture_init_error(firsterr))
			goto unwind;
	}
	if (stat_interval > 0) {
		firsterr = torture_create_kthread(scf_torture_stats, NULL, scf_torture_stats_task);
		if (torture_init_error(firsterr))
			goto unwind;
	}

	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	scf_torture_cleanup();
	if (shutdown_secs) {
		WARN_ON(!IS_MODULE(CONFIG_SCF_TORTURE_TEST));
		kernel_power_off();
	}
	return firsterr;
}

module_init(scf_torture_init);
module_exit(scf_torture_cleanup);
