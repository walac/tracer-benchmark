# Linux Kernel Module: Benchmarking `local_irq` and `preempt` functions

## Overview

This kernel module measures the performance overhead introduced by
`local_irq_disable/enable()` and `preempt_disable/enable()` operations. Its
primary purpose is to quantify the latency impact of enabling the tracepoints
introduced in these functions.

## Key Features

- **Per-CPU Benchmarking**: Spawns one kernel thread per CPU.
- **Sampling**: Each thread performs a configurable number of samples
  (`nr_samples`), where it times:
  1. `local_irq_disable()` + `local_irq_enable()`
  2. `preempt_disable()` + `preempt_enable()`
- **Statistics Collection**: For each CPU and for the global system:
  - Median
  - Average
  - Maximum
  - Average of the highest N samples (`max_avg`)
  - Arbitrary percentile
- **Results Exported via `debugfs`**

## How It Works

1. When triggered via debugfs (`benchmark` file), one thread per CPU is started.
2. Threads simultaneously benchmark the IRQ and preemption toggling by measuring
   the time (in nanoseconds) before and after each pair of operations.
3. Per-CPU statistics are computed.
4. The results are aggregated system-wide and stored in memory for inspection.

## `debugfs` Interface

After module insertion, a directory is created under
`/sys/kernel/debug/` (usually
`/sys/kernel/debug/tracerbenchmark/`).

### Configuration Files

| File         | Description                         |
|--------------|-------------------------------------|
| `nr_samples` | Number of samples per CPU (default: 10,000) |
| `nr_highest` | Number of highest samples to track for `max_avg` computation (default: 100) |

These are writable `size_t` files.

### Trigger Files

| File         | Description                                     |
|--------------|-------------------------------------------------|
| `benchmark`  | Write anything to start the full benchmark run  |
| `percentile` | Write a value between `1` and `100` to compute that percentile |

Both are write-only.

### Output Files (Read-only)

Organized in two subdirectories: `irq/` and `preempt/`

Each contains:

| File         | Description                              |
|--------------|------------------------------------------|
| `median`     | Median time (ns)                         |
| `average`    | Average time (ns)                        |
| `max`        | Maximum single sample (ns)               |
| `max_avg`    | Average of top-N longest samples (ns)    |
| `percentile` | Nth percentile from last `percentile` run|

### Example Usage

```bash
# Mount debugfs if not already done
mount -t debugfs none /sys/kernel/debug

# Change into the module's debugfs directory
cd /sys/kernel/debug/tracerbenchmark/

# Configure parameters
echo 50000 > nr_samples
echo 250 > nr_highest

# Start the benchmark
echo 1 > benchmark

# View results
cat irq/average
cat preempt/max

# Calculate the 99th percentile
echo 99 > percentile

# View the percentile result
cat irq/percentile
cat preempt/percentile
```

## Dependencies

Ensure that `debugfs` is mounted and accessible. The module relies on debugfs
for both configuration and result output.

