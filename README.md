# tracerbench

A Linux kernel module that benchmarks the latency overhead of
`local_irq_disable/enable()` and `preempt_disable/enable()` operations. Its
primary purpose is to quantify the performance impact of enabling the
tracepoints in these kernel functions.

## Prerequisites

- Kernel headers for the running kernel (for out-of-tree module build)
- `CONFIG_DEBUG_FS` enabled in the kernel configuration
- Root access (for loading the module and accessing debugfs)

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

Writing to the `benchmark` file triggers a blocking, system-wide
benchmark run. The write does not return until all CPUs have finished
sampling and the results have been aggregated. Only one benchmark can
run at a time; concurrent writes are serialized.

Before spawning threads, the current `nr_samples` and `nr_highest`
values are snapshotted so that configuration changes via debugfs do
not affect a running benchmark. Note that `nr_highest` is clamped to
`nr_samples` if it exceeds it.

Each CPU then simultaneously collects `nr_samples` timing measurements
of the disable/enable pairs and computes per-CPU statistics (median,
average, max). After all CPUs finish, results are aggregated into the
global statistics:

- **median**: median of per-CPU medians
- **avg**: mean of per-CPU averages (valid because sample counts are
  equal across CPUs)
- **max**: maximum across all CPUs
- **max_avg**: average of the globally highest samples, tracked via
  min-heaps that each CPU feeds into

### Percentile

Writing a value (1-100) to the `percentile` file collects fresh samples
on the **current CPU only**, sorts them, and computes the nth percentile.
This is independent of the benchmark flow and uses the live `nr_samples`
value (not the snapshotted benchmark value), so changing `nr_samples`
between a benchmark and a percentile run will affect the number of
samples collected.

## debugfs Interface

After loading the module, the following tree is created under debugfs:

```
/sys/kernel/debug/tracerbench/
    nr_samples          (rw)  configuration
    nr_highest          (rw)  configuration
    benchmark           (-w)  trigger
    percentile          (-w)  trigger
    irq/
        median          (r-)  result
        average         (r-)  result
        max             (r-)  result
        max_avg         (r-)  result
        percentile      (r-)  result
    preempt/
        median          (r-)  result
        average         (r-)  result
        max             (r-)  result
        max_avg         (r-)  result
        percentile      (r-)  result
```

### Configuration Files

| File         | Description                                                  |
|--------------|--------------------------------------------------------------|
| `nr_samples` | Number of samples per CPU (default: 10,000)                  |
| `nr_highest` | Number of highest samples to track for `max_avg` (default: 100) |

Both are readable and writable. Zero values are rejected with
`-EINVAL`.

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

# Run the benchmark (blocks until complete)
echo 1 > benchmark

# View results
cat irq/average
cat preempt/max

# Calculate the 99th percentile (runs on current CPU only)
echo 99 > percentile
cat irq/percentile
cat preempt/percentile
```

## Design

The module uses `smpboot_register_percpu_thread()` to create worker
threads and holds `cpus_read_lock` for the entire run to prevent CPU
hotplug from changing the set of online CPUs mid-benchmark.

All threads wait on a `completion` barrier so they begin sampling at the
same time. The `time_diff()` macro uses token-pasting to expand
`local_irq` into `local_irq_disable()`/`local_irq_enable()` (and
likewise for `preempt`), measuring the elapsed time via `ktime_get_ns()`.

Per-CPU statistics are computed locally. Sorting (for median and max)
is done in a single pass via `median_and_max()`. Each CPU then feeds
its top `nr_highest` samples into shared min-heaps under a mutex. The
min-heap root is always the smallest of the top-N values, so new
samples only replace it if they are larger, efficiently tracking the
globally worst-case latencies without requiring O(total_samples) global
memory.

Global aggregation computes median-of-medians, mean-of-means (valid
because all CPUs contribute equal sample counts), and max-of-maxes.
The `max_avg` statistic is the arithmetic mean of the min-heap contents.

Memory management uses RAII-style `__free(kvfree)` annotations for
automatic cleanup of per-thread buffers. Heap memory is managed
manually in `benchmark_write()` because ownership is transferred out
via `no_free_ptr()` in `init_heaps()`. Overflow-safe arithmetic
(`check_add_overflow()`, `check_mul_overflow()`) is used throughout
sample accumulation.

## Unloading

```bash
sudo rmmod tracerbench
```

## License

GPL-2.0-only

