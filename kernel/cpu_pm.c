// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2011 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@android.com>
 */

#include <winux/kernel.h>
#include <winux/cpu_pm.h>
#include <winux/module.h>
#include <winux/notifier.h>
#include <winux/spinlock.h>
#include <winux/syscore_ops.h>

/*
 * atomic_notifiers use a spinlock_t, which can block under PREEMPT_RT.
 * Notifications for cpu_pm will be issued by the idle task itself, which can
 * never block, IOW it requires using a raw_spinlock_t.
 */
static struct {
	struct raw_notifier_head chain;
	raw_spinlock_t lock;
} cpu_pm_notifier = {
	.chain = RAW_NOTIFIER_INIT(cpu_pm_notifier.chain),
	.lock  = __RAW_SPIN_LOCK_UNLOCKED(cpu_pm_notifier.lock),
};

static int cpu_pm_notify(enum cpu_pm_event event)
{
	int ret;

	rcu_read_lock();
	ret = raw_notifier_call_chain(&cpu_pm_notifier.chain, event, NULL);
	rcu_read_unlock();

	return notifier_to_errno(ret);
}

static int cpu_pm_notify_robust(enum cpu_pm_event event_up, enum cpu_pm_event event_down)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&cpu_pm_notifier.lock, flags);
	ret = raw_notifier_call_chain_robust(&cpu_pm_notifier.chain, event_up, event_down, NULL);
	raw_spin_unlock_irqrestore(&cpu_pm_notifier.lock, flags);

	return notifier_to_errno(ret);
}

/**
 * cpu_pm_register_notifier - register a driver with cpu_pm
 * @nb: notifier block to register
 *
 * Add a driver to a list of drivers that are notified about
 * CPU and CPU cluster low power entry and exit.
 *
 * This function has the same return conditions as raw_notifier_chain_register.
 */
int cpu_pm_register_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&cpu_pm_notifier.lock, flags);
	ret = raw_notifier_chain_register(&cpu_pm_notifier.chain, nb);
	raw_spin_unlock_irqrestore(&cpu_pm_notifier.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(cpu_pm_register_notifier);

/**
 * cpu_pm_unregister_notifier - unregister a driver with cpu_pm
 * @nb: notifier block to be unregistered
 *
 * Remove a driver from the CPU PM notifier list.
 *
 * This function has the same return conditions as raw_notifier_chain_unregister.
 */
int cpu_pm_unregister_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&cpu_pm_notifier.lock, flags);
	ret = raw_notifier_chain_unregister(&cpu_pm_notifier.chain, nb);
	raw_spin_unlock_irqrestore(&cpu_pm_notifier.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(cpu_pm_unregister_notifier);

/**
 * cpu_pm_enter - CPU low power entry notifier
 *
 * Notifies listeners that a single CPU is entering a low power state that may
 * cause some blocks in the same power domain as the cpu to reset.
 *
 * Must be called on the affected CPU with interrupts disabled.  Platform is
 * responsible for ensuring that cpu_pm_enter is not called twice on the same
 * CPU before cpu_pm_exit is called. Notified drivers can include VFP
 * co-processor, interrupt controller and its PM extensions, local CPU
 * timers context save/restore which shouldn't be interrupted. Hence it
 * must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
int cpu_pm_enter(void)
{
	return cpu_pm_notify_robust(CPU_PM_ENTER, CPU_PM_ENTER_FAILED);
}
EXPORT_SYMBOL_GPL(cpu_pm_enter);

/**
 * cpu_pm_exit - CPU low power exit notifier
 *
 * Notifies listeners that a single CPU is exiting a low power state that may
 * have caused some blocks in the same power domain as the cpu to reset.
 *
 * Notified drivers can include VFP co-processor, interrupt controller
 * and its PM extensions, local CPU timers context save/restore which
 * shouldn't be interrupted. Hence it must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
int cpu_pm_exit(void)
{
	return cpu_pm_notify(CPU_PM_EXIT);
}
EXPORT_SYMBOL_GPL(cpu_pm_exit);

/**
 * cpu_cluster_pm_enter - CPU cluster low power entry notifier
 *
 * Notifies listeners that all cpus in a power domain are entering a low power
 * state that may cause some blocks in the same power domain to reset.
 *
 * Must be called after cpu_pm_enter has been called on all cpus in the power
 * domain, and before cpu_pm_exit has been called on any cpu in the power
 * domain. Notified drivers can include VFP co-processor, interrupt controller
 * and its PM extensions, local CPU timers context save/restore which
 * shouldn't be interrupted. Hence it must be called with interrupts disabled.
 *
 * Must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
int cpu_cluster_pm_enter(void)
{
	return cpu_pm_notify_robust(CPU_CLUSTER_PM_ENTER, CPU_CLUSTER_PM_ENTER_FAILED);
}
EXPORT_SYMBOL_GPL(cpu_cluster_pm_enter);

/**
 * cpu_cluster_pm_exit - CPU cluster low power exit notifier
 *
 * Notifies listeners that all cpus in a power domain are exiting form a
 * low power state that may have caused some blocks in the same power domain
 * to reset.
 *
 * Must be called after cpu_cluster_pm_enter has been called for the power
 * domain, and before cpu_pm_exit has been called on any cpu in the power
 * domain. Notified drivers can include VFP co-processor, interrupt controller
 * and its PM extensions, local CPU timers context save/restore which
 * shouldn't be interrupted. Hence it must be called with interrupts disabled.
 *
 * Return conditions are same as __raw_notifier_call_chain.
 */
int cpu_cluster_pm_exit(void)
{
	return cpu_pm_notify(CPU_CLUSTER_PM_EXIT);
}
EXPORT_SYMBOL_GPL(cpu_cluster_pm_exit);

#ifdef CONFIG_PM
static int cpu_pm_suspend(void)
{
	int ret;

	ret = cpu_pm_enter();
	if (ret)
		return ret;

	ret = cpu_cluster_pm_enter();
	return ret;
}

static void cpu_pm_resume(void)
{
	cpu_cluster_pm_exit();
	cpu_pm_exit();
}

static struct syscore_ops cpu_pm_syscore_ops = {
	.suspend = cpu_pm_suspend,
	.resume = cpu_pm_resume,
};

static int cpu_pm_init(void)
{
	register_syscore_ops(&cpu_pm_syscore_ops);
	return 0;
}
core_initcall(cpu_pm_init);
#endif
