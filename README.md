# tracerbench

A Linux kernel module that benchmarks the latency overhead of
`local_irq_disable/enable()` and `preempt_disable/enable()` operations. Its
primary purpose is to quantify the performance impact of enabling the
tracepoints in these kernel functions.

## Building

```bash
make -C /lib/modules/$(uname -r)/build M=$PWD modules
```

## Loading

```bash
sudo insmod tracerbench.ko
```

## Key Features

- **Per-CPU benchmarking**: spawns one kernel thread per online CPU
- **Configurable sampling**: each thread performs `nr_samples` timing
  measurements of:
  1. `local_irq_disable()` + `local_irq_enable()`
  2. `preempt_disable()` + `preempt_enable()`
- **Statistical analysis**: median, average, maximum, and average of the
  top-N highest samples (`max_avg`)
- **Percentile computation**: independent single-CPU operation for
  arbitrary percentile calculation
- **Results exported via debugfs**

## How It Works

### Benchmark

1. Writing to the debugfs `benchmark` file snapshots the current
   `nr_samples` and `nr_highest` configuration so that mid-flight
   changes do not affect a running benchmark.
2. Per-CPU threads are spawned via `smpboot_register_percpu_thread()`
   inside a `cpus_read_lock` guard to prevent CPU hotplug during the
   run.
3. All threads wait on a completion barrier, then start sampling
   simultaneously.
4. Each thread measures ns-level latency using `ktime_get_ns()` around
   each disable/enable pair and computes per-CPU statistics (median,
   average, max).
5. Each CPU feeds its top `nr_highest` samples into shared min-heaps
   (one for IRQ, one for preempt) under a mutex. The min-heap
   efficiently tracks the globally highest samples: new values only
   replace the heap root if they are larger.
6. After all threads complete, global statistics are aggregated:
   - **median**: median of per-CPU medians
   - **avg**: mean of per-CPU averages
   - **max**: maximum across all CPUs
   - **max_avg**: average of the global min-heap contents (i.e., the
     mean of the worst-case samples across all CPUs)

### Percentile

Writing a value (1-100) to the `percentile` file collects fresh samples
on the **current CPU only**, sorts them, and computes the nth percentile.
This is independent of the benchmark flow.

## debugfs Interface

After loading the module, a directory is created at
`/sys/kernel/debug/tracerbench/`.

### Configuration Files

| File         | Description                                                  |
|--------------|--------------------------------------------------------------|
| `nr_samples` | Number of samples per CPU (default: 10,000)                  |
| `nr_highest` | Number of highest samples to track for `max_avg` (default: 100) |

Both are readable and writable.

### Trigger Files (write-only)

| File         | Description                                                     |
|--------------|-----------------------------------------------------------------|
| `benchmark`  | Write anything to start the full per-CPU benchmark run          |
| `percentile` | Write a value between 1 and 100 to compute that percentile      |

### Result Files (read-only)

Results are organized in two subdirectories: `irq/` and `preempt/`.

Each contains:

| File         | Description                                    |
|--------------|------------------------------------------------|
| `median`     | Median latency (ns)                            |
| `average`    | Average latency (ns)                           |
| `max`        | Maximum single-sample latency (ns)             |
| `max_avg`    | Average of the top-N highest samples (ns)      |
| `percentile` | Nth percentile from the last `percentile` run  |

### Example Usage

```bash
# Mount debugfs if not already done
mount -t debugfs none /sys/kernel/debug

cd /sys/kernel/debug/tracerbench/

# Configure parameters
echo 50000 > nr_samples
echo 250 > nr_highest

# Run the benchmark
echo 1 > benchmark

# View results
cat irq/average
cat preempt/max

# Calculate the 99th percentile (runs on current CPU only)
echo 99 > percentile
cat irq/percentile
cat preempt/percentile
```

## Unloading

```bash
sudo rmmod tracerbench
```

## License

GPL-2.0-only

