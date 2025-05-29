This module measures the performance impact of local_irq_disable/enable and
preempt_disable/enable operations. The primary purpose is to quantify the
overhead introduced by the irqsoff and preempt tracers in the kernel.

Implementation:
* Creates one worker thread per CPU
* Each thread performs the following sequence "nr_samples" times:
  1. Disables local interrupts (local_irq_disable)
  2. Enables local interrupts (local_irq_enable)
  3. Disables preemption (preempt_disable)
  4. Enables preemption (preempt_enable)
* Tracks execution times and stores them across all CPUs

The collected data helps analyze the worst-case latency impacts of these
operations when tracing is active.

