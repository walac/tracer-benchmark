// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Red Hat Inc., Wander Lairson Costa
 *
 * This module measures the performance impact of local_irq_disable/enable and
 * preempt_disable/enable operations. The primary purpose is to quantify the
 * overhead introduced by the irqsoff and preempt tracers in the kernel.
 *
 * Implementation:
 * - Creates one worker thread per CPU
 * - Each thread performs the following sequence "nr_samples" times:
 *   1. Disables local interrupts (local_irq_disable)
 *   2. Enables local interrupts (local_irq_enable)
 *   3. Disables preemption (preempt_disable)
 *   4. Enables preemption (preempt_enable)
 * - Tracks execution times and stores them across all CPUs
 *
 * The collected data helps analyze the worst-case latency impacts of these
 * operations when tracing is active.
 */

//#define DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME " %s: " fmt, current->comm
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/moduleparam.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/ktime.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/smpboot.h>
#include <linux/printk.h>
#include <linux/completion.h>
#include <linux/overflow.h>
#include <linux/sort.h>
#include <linux/minmax.h>
#include <linux/cpuhplock.h>

static ulong nr_samples = 0;
module_param(nr_samples, ulong, 0600);
MODULE_PARM_DESC(nr_samples, "Number of total samples");

struct statistics {
	u64 median;
	u64 average;
	u64 max;
	u64 *data;
};

#define STATISTICS_INITIALIZER {	\
	.median		= 0,		\
	.average	= 0,		\
	.max		= 0,		\
	.data		= NULL,		\
}

struct percpu_data {
	struct statistics irqsoff;
	struct statistics preempt;
	bool should_run;
};

static void swp(void *a, void *b, int size)
{
	u64 tmp;
	u64 *x = a, *y = b;

	tmp = *x;
	*x = *y;
	*y = tmp;
}

static int cmp(const void *a, const void *b)
{
	const u64 x = *(const u64 *) a;
	const u64 y = *(const u64 *) b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static u64 get_median(u64 *p, size_t n)
{
	const size_t pos = n / 2;

	sort(p, n, sizeof(u64), cmp, swp);

	if (n % 2)
		return p[pos];

	return (p[pos] + p[pos-1]) / 2;
}

#ifdef DEBUG
static void print_values(u64 *p, size_t n, const char *name)
{
	pr_debug("%s:", name);
	for (size_t i = 0; i < n; ++i)
		pr_cont(" %llu", p[i]);
	pr_cont("\n");
}
#else
static inline void print_values(u64 *p, size_t n, const char *name)
{}
#endif

static DEFINE_PER_CPU(struct percpu_data, data) = {
	.irqsoff	= STATISTICS_INITIALIZER,
	.preempt	= STATISTICS_INITIALIZER,
	.should_run	= true,
};

static DECLARE_COMPLETION(threads_should_run);

static void sample_thread_fn(unsigned int cpu)
{
	u64 *irqsoff, *preempt = NULL;
	struct percpu_data *my_data;
	u64 start, end;

	pr_debug("sample thread starting\n");

	irqsoff = kvmalloc_array(nr_samples, sizeof(u64), GFP_KERNEL);
	if (!irqsoff)
		goto out;

	preempt = kvmalloc_array(nr_samples, sizeof(u64), GFP_KERNEL);
	if (!preempt) {
		kvfree(irqsoff);
		goto out;
	}

	for (ulong i = 0; i < nr_samples; ++i) {
		start = ktime_get_ns();
		local_irq_disable();
		local_irq_enable();
		end = ktime_get_ns();
		irqsoff[i] = end - start;

		start = ktime_get_ns();
		preempt_disable();
		preempt_enable();
		end = ktime_get_ns();
		preempt[i] = end - start;
	}

out:
	my_data = get_cpu_ptr(&data);
	my_data->irqsoff.data = irqsoff;
	my_data->preempt.data = preempt;
	/*
	 * Avoid we reenter the function before the main task call kthread_stop
	 */
	my_data->should_run = false;
	put_cpu_ptr(&data);
}

static void sample_thread_cleanup(unsigned int cpu, bool online)
{
	struct percpu_data *my_data;
	struct statistics *irqsoff_stat, *preempt_stat;
	u64 irqsoff_total = 0, preempt_total = 0;

	pr_debug("sample thread cleanup\n");

	my_data = get_cpu_ptr(&data);
	irqsoff_stat = &my_data->irqsoff;
	preempt_stat = &my_data->preempt;

	if (WARN_ON(!online))
		goto out;

	for (ulong i = 0; i < nr_samples; ++i) {
		WARN_ON(check_add_overflow(irqsoff_total,
						     irqsoff_stat->data[i], &irqsoff_total));
		WARN_ON(check_add_overflow(preempt_total,
						     preempt_stat->data[i], &preempt_total));
	}

	irqsoff_stat->median = get_median(irqsoff_stat->data, nr_samples);
	preempt_stat->median = get_median(preempt_stat->data, nr_samples);
	irqsoff_stat->average = irqsoff_total / nr_samples;
	preempt_stat->average = preempt_total / nr_samples;
	irqsoff_stat->max = irqsoff_stat->data[nr_samples-1];
	preempt_stat->max = preempt_stat->data[nr_samples-1];

out:
	kvfree(irqsoff_stat->data);
	kvfree(preempt_stat->data);
	put_cpu_ptr(&data);
}

static int sample_thread_should_run(unsigned int cpu)
{
	const struct percpu_data *my_data = get_cpu_ptr(&data);
	const bool should_run = my_data->should_run;

	put_cpu_ptr(&data);
	return should_run;
}

static DEFINE_PER_CPU(struct task_struct *, ktracer);

static struct smp_hotplug_thread sample_thread = {
	.store			= &ktracer,
	.thread_fn		= sample_thread_fn,
	.cleanup		= sample_thread_cleanup,
	.thread_should_run	= sample_thread_should_run,
	.thread_comm		= "ktracer/%u",
};

static int __init mod_init(void)
{
	u64 irqsoff_total = 0;
	u64 preempt_total = 0;
	u64 *irqsoff_medians, *preempt_medians;
	u64 irqsoff_median, preempt_median;
	u64 irqsoff_max = 0, preempt_max = 0;
	size_t i, nr_cpus;
	int ret = 0;
	unsigned int cpu;

	if (!nr_samples) {
		pr_err_once("nr_samples module parameter not set\n");
		return -EINVAL;
	}

	ret = smpboot_register_percpu_thread(&sample_thread);
	if (ret)
		return ret;

	/*
	 * we use the completion here to signal the percpu threads to make
	 * sure they start the same time
	 */
	complete_all(&threads_should_run);

	smpboot_unregister_percpu_thread(&sample_thread);

	cpus_read_lock();
	nr_cpus = num_online_cpus();

	irqsoff_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
	if (!irqsoff_medians) {
		cpus_read_unlock();
		return -ENOMEM;
	}

	preempt_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
	if (!preempt_medians) {
		kfree(irqsoff_medians);
		cpus_read_unlock();
		return -ENOMEM;
	}

	i = 0;
	for_each_online_cpu(cpu) {
		const struct percpu_data *my_data = per_cpu_ptr(&data, cpu);

		/*
		 * compute the average of the averages. Since the number of samples
		 * is equal for all average, the math works
		 */
		WARN_ON(check_add_overflow(irqsoff_total, my_data->irqsoff.average,
						     &irqsoff_total));
		WARN_ON(check_add_overflow(preempt_total, my_data->preempt.average,
						     &preempt_total));

		irqsoff_max = max(irqsoff_max, my_data->irqsoff.max);
		preempt_max = max(preempt_max, my_data->preempt.max);
		irqsoff_medians[i] = my_data->irqsoff.median;
		preempt_medians[i] = my_data->preempt.median;
		++i;
	}

	cpus_read_unlock();

	irqsoff_median = get_median(irqsoff_medians, nr_cpus);
	preempt_median = get_median(preempt_medians, nr_cpus);

	print_values(irqsoff_medians, nr_cpus, "irqsoff medians");
	print_values(preempt_medians, nr_cpus, "preempt medians");

	pr_info("irqsoff: average=%llu max=%llu median=%llu\n",
		irqsoff_total / 2, irqsoff_max, irqsoff_median);
	pr_info("preempt: average=%llu max=%llu median=%llu\n",
		preempt_total / 2, preempt_max, preempt_median);

	kfree(irqsoff_medians);
	kfree(preempt_medians);
	return 0;
}

static void __exit mod_exit(void)
{
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wander Lairson Costa");
MODULE_DESCRIPTION("Stress the irqoffs and preempt tracers");
