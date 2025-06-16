// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Red Hat Inc., Wander Lairson Costa
 *
 * This module measures the performance impact of local_irq_disable/enable and
 * preempt_disable/enable operations. The primary purpose is to quantify the
 * overhead introduced by the irq and preempt tracerpoints in the kernel.
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
#include <linux/kstrtox.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/min_heap.h>

static size_t nr_samples = 10000;
static size_t nr_highest = 100;
static size_t cached_nr_highest;

DEFINE_MIN_HEAP(u64, u64_min_heap);

struct statistics {
	u64 median;
	u64 avg;
	u64 max;
	u64 max_avg;
	u64 percentile;
};

#define NR_STATISTICS (sizeof(struct statistics)/sizeof(u64))

#define STATISTICS_INITIALIZER {	\
	.median		= 0,		\
	.avg		= 0,		\
	.max		= 0,		\
	.max_avg	= 0,		\
	.percentile	= 0,		\
}

struct percpu_data {
	struct statistics irq;
	struct statistics preempt;
	bool should_run;
};

struct debugfs_entry {
	const char *filename;
	void *value;
};

struct debugfs_results {
	const char *subdir;
	struct debugfs_entry values[NR_STATISTICS];
};

static DEFINE_PER_CPU(struct percpu_data, data) = {
	.irq		= STATISTICS_INITIALIZER,
	.preempt	= STATISTICS_INITIALIZER,
	.should_run	= true,
};

static DECLARE_COMPLETION(threads_should_run);
static DEFINE_MUTEX(heap_lock);
static struct u64_min_heap irq_heap;
static struct u64_min_heap preempt_heap;

static struct statistics irq_stat = STATISTICS_INITIALIZER;
static struct statistics preempt_stat = STATISTICS_INITIALIZER;


static const struct debugfs_entry __initdata configs[] = {
	{"nr_samples", &nr_samples	},
	{"nr_highest", &nr_highest	},
};

static const struct debugfs_results debugfs_result_files[] = {
	{
		"irq",
		{
			{"median",	&irq_stat.median	},
			{"average",	&irq_stat.avg		},
			{"max",		&irq_stat.max		},
			{"max_avg",	&irq_stat.max_avg	},
			{"percentile",	&irq_stat.percentile	},
		},
	},
	{
		"preempt",
		{
			{"median",	&preempt_stat.median	},
			{"average",	&preempt_stat.avg	},
			{"max",		&preempt_stat.max	},
			{"max_avg",	&preempt_stat.max_avg	},
			{"percentile",	&preempt_stat.percentile},
		},
	},
};

static void u64_swp(void *a, void *b, int size)
{
	u64 tmp;
	u64 *x = a, *y = b;

	tmp = *x;
	*x = *y;
	*y = tmp;
}

static int u64_cmp(const void *a, const void *b)
{
	const u64 x = *(const u64 *) a;
	const u64 y = *(const u64 *) b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static bool min_heap_less(const void *lhs, const void *rhs, void *args)
{
	return u64_cmp(lhs, rhs) < 0;
}

static void min_heap_swp(void *lhs, void *rhs, void *args)
{
	u64_swp(lhs, rhs, sizeof(u64));
}

static const struct min_heap_callbacks cbs = {
	.less	= min_heap_less,
	.swp	= min_heap_swp,
};

static void add_samples(struct u64_min_heap *h, const u64 *samples, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		if (!min_heap_full_inline(h))
			min_heap_push_inline(h, &samples[i], &cbs, NULL);
		else if (samples[i] > h->data[0]) {
			h->data[0] = samples[i];
			min_heap_sift_down_inline(h, 0, &cbs, NULL);
		}
	}
}

static u64 compute_heap_average(struct u64_min_heap *h)
{
	u64 total = 0;

	for (size_t i = 0; i < h->nr; ++i)
		WARN_ON(check_add_overflow(total, h->data[i], &total));

	return total / h->nr;
}

static u64 nth_percentile(u64 percentile, u64 *p, size_t n)
{
	size_t tmp, pos;

	WARN_ON(check_mul_overflow((u64) n, percentile, &tmp));
	pos = div64_ul(tmp, 100);
	pos = clamp(pos, 0, n - 1);
	sort(p, n, sizeof(u64), u64_cmp, u64_swp);
	return p[pos];
}

static u64 median_and_max(u64 *p, size_t n, u64 *max_val)
{
	const size_t pos = n / 2;

	sort(p, n, sizeof(u64), u64_cmp, u64_swp);

	if (max_val)
		*max_val = p[n-1];

	if (n % 2)
		return p[pos];

	return (p[pos] + p[pos-1]) / 2;
}

static int init_heaps(void)
{
	/*
	 * one extra space to make it easier to compute when
	 * the heap is full
	 */
	const size_t n = cached_nr_highest + 1;
	void *p1 __free(kvfree) = NULL;
	void *p2 __free(kvfree) = NULL;

	p1 = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	p2 = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!p1 || !p2)
		return -ENOMEM;

	min_heap_init_inline(&irq_heap, no_free_ptr(p1), n);
	min_heap_init_inline(&preempt_heap, no_free_ptr(p2), n);
	return 0;
}

static void compute_statistics(struct percpu_data *my_data, u64 *irq_data,
			       u64 *preempt_data, size_t n)
{
	struct statistics *irq = &my_data->irq;
	struct statistics *preempt = &my_data->preempt;
	u64 irq_total = 0, preempt_total = 0;

	for (size_t i = 0; i < n; ++i) {
		WARN_ON(check_add_overflow(irq_total, irq_data[i], &irq_total));
		WARN_ON(check_add_overflow(preempt_total, preempt_data[i], &preempt_total));
	}

	irq->median	= median_and_max(irq_data, n, &irq->max);
	irq->avg	= irq_total / n;

	preempt->median	= median_and_max(preempt_data, n, &preempt->max);
	preempt->avg	= preempt_total / n;
}

#define time_diff(call) ({		\
	const u64 ts = ktime_get_ns();	\
	call##_disable();		\
	call##_enable();		\
	ktime_get_ns() - ts;		\
})

static void collect_data(u64 *irq, u64 *preempt, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		irq[i] = time_diff(local_irq);
		preempt[i] = time_diff(preempt);
	}
}

static void sample_thread_fn(unsigned int cpu)
{
	u64 *irq __free(kvfree) = NULL;
	u64 *preempt __free(kvfree) = NULL;
	struct percpu_data *my_data;
	const size_t n = READ_ONCE(nr_samples);
	const size_t nh = READ_ONCE(cached_nr_highest);

	pr_debug("sample thread starting\n");

	irq = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	preempt = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (irq && preempt) {
		wait_for_completion(&threads_should_run);
		collect_data(irq, preempt, n);
	}

	my_data = get_cpu_ptr(&data);
	compute_statistics(my_data, irq, preempt, n);

	/*
	 * Avoid we reenter the function before the main task call kthread_stop
	 */
	my_data->should_run = false;
	put_cpu_ptr(&data);

	guard(mutex)(&heap_lock);
	add_samples(&irq_heap, irq + (n - nh), nh);
	add_samples(&preempt_heap, preempt + (n - nh), nh);
}

static int sample_thread_should_run(unsigned int cpu)
{
	return this_cpu_ptr(&data)->should_run;
}

static void sample_thread_cleanup(unsigned int cpu, bool online)
{
	this_cpu_ptr(&data)->should_run = true;
}

static DEFINE_PER_CPU(struct task_struct *, ktracer);

static struct smp_hotplug_thread sample_thread = {
	.store			= &ktracer,
	.thread_fn		= sample_thread_fn,
	.thread_should_run	= sample_thread_should_run,
	.cleanup		= sample_thread_cleanup,
	.thread_comm		= "ktracer/%u",
};

static int run_benchmark(void)
{
	u64 *irq_medians __free(kfree) = NULL;
	u64 *preempt_medians __free(kfree) = NULL;
	size_t i, nr_cpus;
	int ret = 0;
	unsigned int cpu;
	u64 irq_total = 0, preempt_total = 0;
	u64 irq_max = 0, preempt_max = 0;

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

		reinit_completion(&threads_should_run);

		nr_cpus = num_online_cpus();

		irq_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
		preempt_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
		if (!irq_medians || !preempt_medians)
			return -ENOMEM;

		i = 0;
		for_each_online_cpu(cpu) {
			const struct percpu_data *my_data = per_cpu_ptr(&data, cpu);

			/*
			 * compute the average of the averages. Since the number of samples
			 * is equal for all average, the math works
			 */
			WARN_ON(check_add_overflow(irq_total, my_data->irq.avg, &irq_total));
			WARN_ON(check_add_overflow(preempt_total, my_data->preempt.avg,
						   &preempt_total));

			irq_max			= max(irq_max, my_data->irq.max);
			irq_medians[i]		= my_data->irq.median;

			preempt_max		= max(preempt_max, my_data->preempt.max);
			preempt_medians[i]	= my_data->preempt.median;
			++i;
		}
	}

	irq_stat.median		= median_and_max(irq_medians, nr_cpus, NULL);
	irq_stat.avg		= irq_total / nr_cpus;
	irq_stat.max		= irq_max;
	irq_stat.max_avg	= compute_heap_average(&irq_heap);

	preempt_stat.median	= median_and_max(preempt_medians, nr_cpus, NULL);
	preempt_stat.avg	= preempt_total / nr_cpus;
	preempt_stat.max	= preempt_max;
	preempt_stat.max_avg	= compute_heap_average(&preempt_heap);

	return 0;
}

static ssize_t benchmark_write(struct file *file, const char __user *buffer,
			      size_t count, loff_t *ppos)
{
	int ret;
	const size_t n = READ_ONCE(nr_samples);

	if (!n) {
		pr_err_once("Number of samples cannot be zero\n");
		return -EINVAL;
	}

	WRITE_ONCE(cached_nr_highest, min(n, READ_ONCE(nr_highest)));

	ret = init_heaps();
	if (ret)
		return ret;

	ret = run_benchmark();
	kvfree(irq_heap.data);
	kvfree(preempt_heap.data);

	return ret ? : count;
}

const struct file_operations benchmark_fops = {
	.owner	= THIS_MODULE,
	.write	= benchmark_write,
	.llseek = default_llseek,
	.open	= simple_open,
};

static ssize_t percentile_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	const size_t n = READ_ONCE(nr_samples);
	int ret;
	unsigned int nth_percent;
	u64 *irq	__free(kvfree) = NULL;
	u64 *preempt	__free(kvfree) = NULL;

	if (!n) {
		pr_err_once("Number of samples cannot be zero\n");
		return -EINVAL;
	}

	ret = kstrtouint_from_user(buffer, count, 0, &nth_percent);
	if (ret)
		return ret;

	if (!nth_percent || nth_percent > 100) {
		pr_err_once("The percentile value can't be zero or greater than 100\n");
		return -EINVAL;
	}

	irq	= kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	preempt = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!irq || !preempt)
		return -ENOMEM;

	pr_debug("Calculating the %uth percentile\n", nth_percent);

	collect_data(irq, preempt, n);
	WRITE_ONCE(irq_stat.percentile, nth_percentile(nth_percent, irq, n));
	WRITE_ONCE(preempt_stat.percentile, nth_percentile(nth_percent, preempt, n));

	return count;
}

const struct file_operations percentile_fops = {
	.owner	= THIS_MODULE,
	.write	= percentile_write,
	.llseek = noop_llseek,
	.open	= simple_open,
};

static void __init create_config_files(struct dentry *parent)
{
	for (size_t i = 0; i < ARRAY_SIZE(configs); ++i)
		debugfs_create_size_t(configs[i].filename, 0644,
				      parent, configs[i].value);
}


static int __init create_stat_files(struct dentry *parent)
{
	static const umode_t mode = 0x444;
	struct dentry *subdir;

	for (size_t i = 0; i < ARRAY_SIZE(debugfs_result_files); ++i) {
		const struct debugfs_results *entries = debugfs_result_files + i;

		subdir = debugfs_create_dir(entries->subdir, parent);
		if (IS_ERR(subdir))
			return PTR_ERR(subdir);

		for (size_t j = 0; j < ARRAY_SIZE(entries->values); ++j) {
			const struct debugfs_entry *entry = entries->values + j;

			debugfs_create_u64(entry->filename, mode, subdir, entry->value);
		}
	}

	return 0;
}

static struct dentry *rootdir;

static int __init mod_init(void)
{
	struct dentry *file;
	int ret;

	compiletime_assert(sizeof(u64)*NR_STATISTICS == sizeof(struct statistics),
			   "struct statistics size is not multiple of u64");

	rootdir = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (IS_ERR(rootdir))
		return PTR_ERR(rootdir);

	file = debugfs_create_file("benchmark", 0200, rootdir, NULL, &benchmark_fops);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err;
	}

	file = debugfs_create_file("percentile", 0200, rootdir, NULL, &percentile_fops);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err;
	}

	create_config_files(rootdir);
	ret = create_stat_files(rootdir);
	if (ret)
		goto err;

	return 0;

err:
	debugfs_remove_recursive(rootdir);
	return ret;
}

static void __exit mod_exit(void)
{
	debugfs_remove_recursive(rootdir);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wander Lairson Costa");
MODULE_DESCRIPTION("Benchmark the irqoffs and preempt tracers");
