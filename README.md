# tracerbench

A Linux kernel module that benchmarks the latency overhead of
`local_irq_disable/enable()`, `preempt_disable/enable()`, and
`local_irq_save/restore()` operations. Its primary purpose is to
quantify the performance impact of enabling the tracepoints in these
kernel functions.  Results are reported in raw CPU cycles
(`get_cycles()`) for minimal timing overhead.

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
  3. `local_irq_save()` + `local_irq_restore()`
- **Statistical analysis**: median, average, maximum, and average of the
  top-N highest samples (`max_avg`)
- **Percentile computation**: configurable nth percentile computed
  during the benchmark run (default: 99th)
- **Results exported via debugfs**

## How It Works

### Benchmark

Writing to the `benchmark` file triggers a blocking, system-wide
benchmark run. The write does not return until all CPUs have finished
sampling and the results have been aggregated. Only one benchmark can
run at a time; concurrent writes are serialized.

Before spawning threads, the current `nr_samples`, `nr_highest`, and
`nth_percentile` values are snapshotted so that configuration changes
via debugfs do not affect a running benchmark. Note that `nr_highest`
is clamped to `nr_samples` if it exceeds it.

Each CPU then simultaneously collects `nr_samples` timing measurements
of the disable/enable pairs and computes per-CPU statistics (median,
average, max, percentile). After all CPUs finish, results are
aggregated into the global statistics:

- **median**: median of per-CPU medians
- **avg**: mean of per-CPU averages (valid because sample counts are
  equal across CPUs)
- **max**: maximum across all CPUs
- **max_avg**: average of the globally highest samples, tracked via
  min-heaps that each CPU feeds into
- **percentile**: maximum of per-CPU nth percentiles (worst-case
  across CPUs)

## debugfs Interface

After loading the module, the following tree is created under debugfs:

```
/sys/kernel/debug/tracerbench/
    nr_samples          (rw)  configuration
    nr_highest          (rw)  configuration
    nth_percentile      (rw)  configuration
    do_work             (rw)  configuration
    benchmark           (-w)  trigger
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
    irq_save/
        median          (r-)  result
        average         (r-)  result
        max             (r-)  result
        max_avg         (r-)  result
        percentile      (r-)  result
```

### Configuration Files

| File             | Description                                                  |
|------------------|--------------------------------------------------------------|
| `nr_samples`     | Number of samples per CPU (default: 10,000)                  |
| `nr_highest`     | Number of highest samples to track for `max_avg` (default: 100) |
| `nth_percentile` | Which percentile to compute, 1-100 (default: 99)             |
| `do_work`        | Simulate critical section work between disable/enable (default: 0) |

`nr_samples`, `nr_highest`, and `nth_percentile` are readable and
writable. Zero values are rejected with `-EINVAL`. `nth_percentile`
also rejects values greater than 100. `do_work` is a boolean toggle
(0 or 1).

### Trigger Files (write-only)

| File         | Description                                                     |
|--------------|-----------------------------------------------------------------|
| `benchmark`  | Write anything to start the full per-CPU benchmark run          |

### Result Files (read-only)

Results are organized in three subdirectories: `irq/`, `preempt/`,
and `irq_save/`.

Each contains:

| File         | Description                                       |
|--------------|---------------------------------------------------|
| `median`     | Median latency (cycles)                           |
| `average`    | Average latency (cycles)                          |
| `max`        | Maximum single-sample latency (cycles)            |
| `max_avg`    | Average of the top-N highest samples (cycles)     |
| `percentile` | Nth percentile (worst-case across CPUs, cycles)   |

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

# View results (values are in CPU cycles)
cat irq/average
cat preempt/max
cat irq_save/average
cat irq/percentile     # 99th percentile (default)

# Run again with simulated critical section work
echo 1 > do_work
echo 1 > benchmark
cat irq/average        # compare with previous run

# Change the percentile and re-run
echo 95 > nth_percentile
echo 1 > benchmark
cat irq/percentile     # now shows 95th percentile
```

## Design

The module uses `smpboot_register_percpu_thread()` to create worker
threads and holds `cpus_read_lock` for the entire run to prevent CPU
hotplug from changing the set of online CPUs mid-benchmark.

All threads wait on a `completion` barrier so they begin sampling at the
same time. The `time_diff()` macro uses token-pasting to expand
`local_irq` into `local_irq_disable()`/`local_irq_enable()` (and
likewise for `preempt`), measuring the elapsed time via `get_cycles()`.
A separate `time_diff_save_restore()` macro handles the
`local_irq_save()`/`local_irq_restore()` pair, which requires a flags
argument.

When `do_work` is enabled, a `noinline` function
`simulate_critical_section()` is called between each disable/enable
pair.  It performs a percpu read-modify-write with a data-dependent
branch and a `current->pid` access, representative of what real
critical sections do.  The cost of this simulated work is included in
the timer overhead measurement and subtracted from each sample, so
results still reflect only the disable/enable cost.

Per-CPU statistics are computed locally. Sorting (for median and max)
is done in a single pass via `median_and_max()`. Each CPU then feeds
its top `nr_highest` samples into shared min-heaps under a mutex. The
min-heap root is always the smallest of the top-N values, so new
samples only replace it if they are larger, efficiently tracking the
globally worst-case latencies without requiring O(total_samples) global
memory.

Global aggregation computes median-of-medians, mean-of-means (valid
because all CPUs contribute equal sample counts), max-of-maxes, and
max-of-percentiles (worst-case nth percentile across CPUs). The
`max_avg` statistic is the arithmetic mean of the min-heap contents.

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

