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
};

#define STATISTICS_INITIALIZER {	\
	.median		= 0,		\
	.average	= 0,		\
	.max		= 0,		\
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

static DEFINE_PER_CPU(struct percpu_data, data) = {
	.irqsoff	= STATISTICS_INITIALIZER,
	.preempt	= STATISTICS_INITIALIZER,
	.should_run	= true,
};

static DECLARE_COMPLETION(threads_should_run);

static void compute_statistics(struct percpu_data *my_data,
			       u64 *irqsoff_data, u64 *preempt_data)
{
	struct statistics *irqsoff = &my_data->irqsoff;
	struct statistics *preempt = &my_data->preempt;
	u64 irqsoff_total = 0, preempt_total = 0;

	for (ulong i = 0; i < nr_samples; ++i) {
		WARN_ON(check_add_overflow(irqsoff_total,
						     irqsoff_data[i], &irqsoff_total));
		WARN_ON(check_add_overflow(preempt_total,
						     preempt_data[i], &preempt_total));
	}

	irqsoff->median		= get_median(irqsoff_data, nr_samples);
	irqsoff->average	= irqsoff_total / nr_samples;
	irqsoff->max		= irqsoff_data[nr_samples-1];

	preempt->median		= get_median(preempt_data, nr_samples);
	preempt->average	= preempt_total / nr_samples;
	preempt->max		= preempt_data[nr_samples-1];
}

#define time_diff(call) ({		\
	const u64 ts = ktime_get_ns();	\
	call##_disable();		\
	call##_enable();		\
	ktime_get_ns() - ts;		\
})

static void sample_thread_fn(unsigned int cpu)
{
	u64 *irqsoff __free(kvfree) = NULL;
	u64 *preempt __free(kvfree) = NULL;
	struct percpu_data *my_data;

	pr_debug("sample thread starting\n");

	irqsoff = kvmalloc_array(nr_samples, sizeof(u64), GFP_KERNEL);
	preempt = kvmalloc_array(nr_samples, sizeof(u64), GFP_KERNEL);
	if (irqsoff && preempt)
		for (ulong i = 0; i < nr_samples; ++i) {
			irqsoff[i] = time_diff(local_irq);
			preempt[i] = time_diff(preempt);
		}

	my_data = get_cpu_ptr(&data);
	compute_statistics(my_data, irqsoff, preempt);

	/*
	 * Avoid we reenter the function before the main task call kthread_stop
	 */
	my_data->should_run = false;
	put_cpu_ptr(&data);
}

static int sample_thread_should_run(unsigned int cpu)
{
	return this_cpu_ptr(&data)->should_run;
}

static DEFINE_PER_CPU(struct task_struct *, ktracer);

static struct smp_hotplug_thread sample_thread = {
	.store			= &ktracer,
	.thread_fn		= sample_thread_fn,
	.thread_should_run	= sample_thread_should_run,
	.thread_comm		= "ktracer/%u",
};

static int __init mod_init(void)
{
	u64 irqsoff_total = 0;
	u64 preempt_total = 0;
	u64 *irqsoff_medians __free(kfree) = NULL;
	u64 *preempt_medians __free(kfree) = NULL;
	u64 irqsoff_median, preempt_median;
	u64 irqsoff_max = 0, preempt_max = 0;
	size_t i, nr_cpus;
	int ret = 0;
	unsigned int cpu;

	if (!nr_samples) {
		pr_err_once("nr_samples module parameter not set\n");
		return -EINVAL;
	}

	scoped_guard(cpus_read_lock) {
		ret = smpboot_register_percpu_thread(&sample_thread);
		if (ret)
			return ret;

		/*
		 * we use the completion here to signal the percpu threads to make
		 * sure they start the same time
		 */
		complete_all(&threads_should_run);

		smpboot_unregister_percpu_thread(&sample_thread);

		nr_cpus = num_online_cpus();

		irqsoff_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
		preempt_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
		if (!irqsoff_medians || !preempt_medians)
			return -ENOMEM;

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
	}

	irqsoff_median = get_median(irqsoff_medians, nr_cpus);
	preempt_median = get_median(preempt_medians, nr_cpus);

	pr_info("irqsoff: average=%llu max=%llu median=%llu\n",
		irqsoff_total / nr_cpus, irqsoff_max, irqsoff_median);
	pr_info("preempt: average=%llu max=%llu median=%llu\n",
		preempt_total / nr_cpus, preempt_max, preempt_median);

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
