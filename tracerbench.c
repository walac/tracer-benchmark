// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Red Hat Inc., Wander Lairson Costa
 *
 * This module measures the performance impact of local_irq_disable/enable and
 * preempt_disable/enable operations. The primary purpose is to quantify the
 * overhead introduced by the IRQ and preempt tracepoints in the kernel.
 *
 * Implementation:
 * - Creates one worker thread per CPU
 * - Each thread performs the following sequence "nr_samples" times:
 *   1. Disables local interrupts (local_irq_disable)
 *   2. Enables local interrupts (local_irq_enable)
 *   3. Disables preemption (preempt_disable)
 *   4. Enables preemption (preempt_enable)
 *   5. Saves and restores local interrupts (local_irq_save/restore)
 * - Tracks execution times (in CPU cycles via get_cycles()) across all CPUs
 *
 * The collected data helps analyze the worst-case latency impacts of these
 * operations when tracing is active.  Results are reported in raw CPU
 * cycles to minimize timing overhead and improve signal-to-noise ratio.
 */

//#define DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME " %s: " fmt, current->comm
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/ktime.h>
#include <linux/timex.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/smpboot.h>
#include <linux/completion.h>
#include <linux/overflow.h>
#include <linux/sort.h>
#include <linux/minmax.h>
#include <linux/cpuhplock.h>
#include <linux/kstrtox.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/min_heap.h>

/*
 * Debugfs-writable configuration parameter with a snapshot for benchmark
 * runs.  The user can change @val at any time via debugfs, so per-CPU
 * worker threads must not read it directly.  Instead, benchmark_write()
 * copies @val into @cached under the benchmark_lock before spawning
 * threads, and the threads read only @cached.  This guarantees every
 * thread sees the same value for the entire run.
 */
struct config {
	size_t val;
	size_t cached;
};

static struct config nr_samples = { .val = 10000 };
static struct config nr_highest = { .val = 100 };
static struct config nth_percentile = { .val = 99 };
static bool do_work;

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
	struct statistics irq_save;
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
	.irq_save	= STATISTICS_INITIALIZER,
	.should_run	= true,
};

static DECLARE_COMPLETION(threads_should_run);
static DEFINE_MUTEX(heap_lock);
static struct u64_min_heap irq_heap;
static struct u64_min_heap preempt_heap;
static struct u64_min_heap irq_save_heap;

static struct statistics irq_stat = STATISTICS_INITIALIZER;
static struct statistics preempt_stat = STATISTICS_INITIALIZER;
static struct statistics irq_save_stat = STATISTICS_INITIALIZER;


/*
 * Generate debugfs get/set accessors and file_operations for a size_t
 * config variable that must be non-zero.
 */
#define DEFINE_CONFIG_ATTR(name)					\
static int name##_get(void *data, u64 *val)				\
{									\
	*val = READ_ONCE(name.val);					\
	return 0;							\
}									\
static int name##_set(void *data, u64 val)				\
{									\
	if (!val)							\
		return -EINVAL;						\
	WRITE_ONCE(name.val, val);					\
	return 0;							\
}									\
DEFINE_DEBUGFS_ATTRIBUTE(name##_fops, name##_get, name##_set, "%llu\n")

DEFINE_CONFIG_ATTR(nr_samples);
DEFINE_CONFIG_ATTR(nr_highest);

static int nth_percentile_get(void *data, u64 *val)
{
	*val = READ_ONCE(nth_percentile.val);
	return 0;
}
static int nth_percentile_set(void *data, u64 val)
{
	if (!val || val > 100)
		return -EINVAL;
	WRITE_ONCE(nth_percentile.val, val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(nth_percentile_fops, nth_percentile_get,
			 nth_percentile_set, "%llu\n");

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
	{
		"irq_save",
		{
			{"median",	&irq_save_stat.median		},
			{"average",	&irq_save_stat.avg		},
			{"max",		&irq_save_stat.max		},
			{"max_avg",	&irq_save_stat.max_avg		},
			{"percentile",	&irq_save_stat.percentile	},
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
	const size_t n = h->nr;

	if (unlikely(!n))
		return 0;

	for (size_t i = 0; i < n; ++i)
		WARN_ON(check_add_overflow(total, h->data[i], &total));

	return total / n;
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
	const size_t n = READ_ONCE(nr_highest.cached) + 1;
	void *p1 __free(kvfree) = NULL;
	void *p2 __free(kvfree) = NULL;
	void *p3 __free(kvfree) = NULL;

	p1 = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	p2 = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	p3 = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!p1 || !p2 || !p3)
		return -ENOMEM;

	min_heap_init_inline(&irq_heap, no_free_ptr(p1), n);
	min_heap_init_inline(&preempt_heap, no_free_ptr(p2), n);
	min_heap_init_inline(&irq_save_heap, no_free_ptr(p3), n);
	return 0;
}

static void compute_one_stat(struct statistics *stat, u64 *samples, size_t n)
{
	size_t pct_idx;
	u64 total = 0;

	for (size_t i = 0; i < n; ++i)
		WARN_ON(check_add_overflow(total, samples[i], &total));

	stat->median	= median_and_max(samples, n, &stat->max);
	stat->avg	= total / n;

	/* median_and_max() sorts the array, so percentile is a simple lookup */
	WARN_ON(check_mul_overflow(n, READ_ONCE(nth_percentile.cached), &pct_idx));
	pct_idx = clamp(pct_idx / 100, 0, n - 1);
	stat->percentile = samples[pct_idx];
}

static void compute_statistics(struct percpu_data *my_data, u64 *irq_data,
			       u64 *preempt_data, u64 *irq_save_data,
			       size_t n)
{
	compute_one_stat(&my_data->irq, irq_data, n);
	compute_one_stat(&my_data->preempt, preempt_data, n);
	compute_one_stat(&my_data->irq_save, irq_save_data, n);
}

/*
 * Simulate a realistic critical section.
 *
 * Back-to-back disable/enable with no work between them never occurs in
 * real kernel code and gives the CPU pipeline, branch predictor, and
 * register allocator an unrealistically easy job.  This function
 * provides a lightweight workload representative of what actual critical
 * sections do:
 *
 *  - Percpu read-modify-write (the most common pattern inside
 *    local_irq_save/restore critical sections, e.g. softirq pending
 *    bits, statistics counters).
 *
 *  - Data-dependent branch that is hard for the branch predictor to
 *    learn, unlike a constant-condition loop.
 *
 *  - Access to current->pid, which touches a different cache line than
 *    the percpu variable, simulating the mixed-locality memory access
 *    patterns typical of real critical sections.
 *
 * Marked noinline so the compiler cannot fold this into the
 * disable/enable macros, which would change their code generation
 * and defeat the purpose of measuring their overhead in a realistic
 * register-pressure context.
 *
 * Controlled by the 'do_work' debugfs toggle (default off).
 */
static DEFINE_PER_CPU(unsigned long, work_counter);

static noinline void simulate_critical_section(void)
{
	unsigned long *p = this_cpu_ptr(&work_counter);
	unsigned long val = READ_ONCE(*p);

	if (val & 0x1)
		val += raw_smp_processor_id();
	else
		val ^= current->pid;

	WRITE_ONCE(*p, val + 1);
}

#define OVERHEAD_SAMPLES 100

/*
 * Measure the cost of the timing infrastructure itself.
 *
 * Take the median of OVERHEAD_SAMPLES back-to-back get_cycles() pairs
 * to get a stable estimate of the timer overhead.  The median resists
 * outliers from interrupts and VM exits.  When do_work is enabled,
 * include the cost of simulate_critical_section() in the overhead so
 * that it is subtracted from the final results, isolating only the
 * disable/enable cost.
 */
static noinline u64 measure_overhead(void)
{
	const bool work = READ_ONCE(do_work);
	u64 samples[OVERHEAD_SAMPLES];
	size_t i;

	for (i = 0; i < OVERHEAD_SAMPLES; ++i) {
		const u64 ts = get_cycles();

		if (work)
			simulate_critical_section();
		samples[i] = get_cycles() - ts;
	}

	return median_and_max(samples, OVERHEAD_SAMPLES, NULL);
}

#define time_diff(call, work) ({	\
	const u64 ts = get_cycles();	\
	call##_disable();		\
	if (work)			\
		simulate_critical_section();\
	call##_enable();		\
	get_cycles() - ts;		\
})

#define time_diff_save_restore(work) ({		\
	unsigned long __flags;			\
	const u64 ts = get_cycles();		\
	local_irq_save(__flags);		\
	if (work)				\
		simulate_critical_section();	\
	local_irq_restore(__flags);		\
	get_cycles() - ts;			\
})

static void subtract_overhead(u64 *samples, size_t n, u64 overhead)
{
	for (size_t i = 0; i < n; ++i) {
		if (samples[i] > overhead)
			samples[i] -= overhead;
		else
			samples[i] = 0;
	}
}

static void collect_data(u64 *irq, u64 *preempt, u64 *irq_save, size_t n)
{
	const bool work = READ_ONCE(do_work);
	u64 overhead;
	size_t i;

	for (i = 0; i < n; ++i)
		irq[i] = time_diff(local_irq, work);

	for (i = 0; i < n; ++i)
		preempt[i] = time_diff(preempt, work);

	for (i = 0; i < n; ++i)
		irq_save[i] = time_diff_save_restore(work);

	overhead = measure_overhead();
	subtract_overhead(irq, n, overhead);
	subtract_overhead(preempt, n, overhead);
	subtract_overhead(irq_save, n, overhead);
}

static void sample_thread_fn(unsigned int cpu)
{
	u64 *irq __free(kvfree) = NULL;
	u64 *preempt __free(kvfree) = NULL;
	u64 *irq_save __free(kvfree) = NULL;
	struct percpu_data *my_data;
	const size_t n = READ_ONCE(nr_samples.cached);
	const size_t nh = READ_ONCE(nr_highest.cached);

	pr_debug("sample thread starting\n");

	irq = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	preempt = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	irq_save = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!irq || !preempt || !irq_save) {
		this_cpu_ptr(&data)->should_run = false;
		return;
	}

	wait_for_completion(&threads_should_run);
	collect_data(irq, preempt, irq_save, n);

	my_data = get_cpu_ptr(&data);
	compute_statistics(my_data, irq, preempt, irq_save, n);

	/*
	 * Avoid we reenter the function before the main task call kthread_stop
	 */
	my_data->should_run = false;
	put_cpu_ptr(&data);

	/*
	 * Feed the top nh samples into the global min-heaps for max_avg
	 * computation. compute_statistics() sorts the arrays via
	 * median_and_max(), so the highest values sit at the tail
	 * and we can select them with a simple pointer offset.
	 */
	guard(mutex)(&heap_lock);
	add_samples(&irq_heap, irq + (n - nh), nh);
	add_samples(&preempt_heap, preempt + (n - nh), nh);
	add_samples(&irq_save_heap, irq_save + (n - nh), nh);
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

static void aggregate_stat(struct statistics *stat, struct u64_min_heap *heap,
			   u64 *medians, u64 total, u64 max_val,
			   u64 max_percentile, size_t nr_cpus)
{
	stat->median		= median_and_max(medians, nr_cpus, NULL);
	stat->avg		= total / nr_cpus;
	stat->max		= max_val;
	stat->max_avg		= compute_heap_average(heap);
	stat->percentile	= max_percentile;
}

static int run_benchmark(void)
{
	u64 *irq_medians __free(kfree) = NULL;
	u64 *preempt_medians __free(kfree) = NULL;
	u64 *irq_save_medians __free(kfree) = NULL;
	size_t i, nr_cpus;
	int ret = 0;
	unsigned int cpu;
	u64 irq_total = 0, preempt_total = 0, irq_save_total = 0;
	u64 irq_max = 0, preempt_max = 0, irq_save_max = 0;
	u64 irq_pct = 0, preempt_pct = 0, irq_save_pct = 0;

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
		irq_save_medians = kmalloc_array(nr_cpus, sizeof(u64), GFP_KERNEL);
		if (!irq_medians || !preempt_medians || !irq_save_medians)
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
			WARN_ON(check_add_overflow(irq_save_total, my_data->irq_save.avg,
						   &irq_save_total));

			irq_max			= max(irq_max, my_data->irq.max);
			irq_pct			= max(irq_pct, my_data->irq.percentile);
			irq_medians[i]		= my_data->irq.median;

			preempt_max		= max(preempt_max, my_data->preempt.max);
			preempt_pct		= max(preempt_pct, my_data->preempt.percentile);
			preempt_medians[i]	= my_data->preempt.median;

			irq_save_max		= max(irq_save_max, my_data->irq_save.max);
			irq_save_pct		= max(irq_save_pct, my_data->irq_save.percentile);
			irq_save_medians[i]	= my_data->irq_save.median;
			++i;
		}
	}

	aggregate_stat(&irq_stat, &irq_heap, irq_medians,
		       irq_total, irq_max, irq_pct, nr_cpus);
	aggregate_stat(&preempt_stat, &preempt_heap, preempt_medians,
		       preempt_total, preempt_max, preempt_pct, nr_cpus);
	aggregate_stat(&irq_save_stat, &irq_save_heap, irq_save_medians,
		       irq_save_total, irq_save_max, irq_save_pct, nr_cpus);

	return 0;
}

static DEFINE_MUTEX(benchmark_lock);

static ssize_t benchmark_write(struct file *file, const char __user *buffer,
			      size_t count, loff_t *ppos)
{
	int ret;
	const size_t n = READ_ONCE(nr_samples.val);

	if (!n) {
		pr_err_once("Number of samples cannot be zero\n");
		return -EINVAL;
	}

	guard(mutex)(&benchmark_lock);

	WRITE_ONCE(nr_samples.cached, n);
	WRITE_ONCE(nr_highest.cached, min(n, READ_ONCE(nr_highest.val)));
	WRITE_ONCE(nth_percentile.cached, READ_ONCE(nth_percentile.val));

	ret = init_heaps();
	if (ret)
		return ret;

	ret = run_benchmark();
	kvfree(irq_heap.data);
	kvfree(preempt_heap.data);
	kvfree(irq_save_heap.data);

	return ret ? : count;
}

static const struct file_operations benchmark_fops = {
	.owner	= THIS_MODULE,
	.write	= benchmark_write,
	.llseek = default_llseek,
	.open	= simple_open,
};

struct debugfs_config {
	const char *filename;
	const struct file_operations *fops;
};

#define CONFIG_ENTRY(name)	{ #name, &name##_fops }

static const struct debugfs_config configs[] = {
	CONFIG_ENTRY(nr_samples),
	CONFIG_ENTRY(nr_highest),
	CONFIG_ENTRY(nth_percentile),
};

static void __init create_config_files(struct dentry *parent)
{
	for (size_t i = 0; i < ARRAY_SIZE(configs); ++i)
		debugfs_create_file_unsafe(configs[i].filename, 0644,
					   parent, NULL, configs[i].fops);
	debugfs_create_bool("do_work", 0644, parent, &do_work);
}


static int __init create_stat_files(struct dentry *parent)
{
	static const umode_t mode = 0444;
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
MODULE_DESCRIPTION("Benchmark the IRQ, preempt, and IRQ save/restore tracers");
