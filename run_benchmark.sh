#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

usage() {
	echo "Usage: $0 [-n nr_samples] [-p percentile] [-t]" >&2
	echo "  -t  enable preemptirq tracepoints before benchmarking" >&2
	exit 1
}

NR_SAMPLES=100000
PERCENTILE=99
ENABLE_TRACEPOINTS=0

while getopts "n:p:th" opt; do
	case $opt in
	n) NR_SAMPLES="$OPTARG" ;;
	p) PERCENTILE="$OPTARG" ;;
	t) ENABLE_TRACEPOINTS=1 ;;
	*) usage ;;
	esac
done

DEBUGFS_ROOT="/sys/kernel/debug"
DEBUGFS="$DEBUGFS_ROOT/tracerbench"

if [ ! -d "$DEBUGFS_ROOT" ]; then
	echo "error: debugfs is not mounted" >&2
	exit 1
fi

if [ ! -d "$DEBUGFS" ]; then
	MODDIR="$(cd "$(dirname "$0")" && pwd)"
	if [ ! -f "$MODDIR/tracerbench.ko" ]; then
		echo "error: tracerbench.ko not found in $MODDIR" >&2
		exit 1
	fi
	insmod "$MODDIR/tracerbench.ko"
fi

KVER="$(uname -r)"
ARCH="$(uname -m)"

TRACING="$DEBUGFS_ROOT/tracing/events/preemptirq"

disable_tracepoints() {
	if [ "$ENABLE_TRACEPOINTS" -eq 1 ] && [ -d "$TRACING" ]; then
		echo 0 > "$TRACING/irq_disable/enable"
		echo 0 > "$TRACING/irq_enable/enable"
		echo 0 > "$TRACING/preempt_disable/enable"
		echo 0 > "$TRACING/preempt_enable/enable"
	fi
}

if [ "$ENABLE_TRACEPOINTS" -eq 1 ] && [ -d "$TRACING" ]; then
	trap disable_tracepoints EXIT
	echo 1 > "$TRACING/irq_disable/enable"
	echo 1 > "$TRACING/irq_enable/enable"
	echo 1 > "$TRACING/preempt_disable/enable"
	echo 1 > "$TRACING/preempt_enable/enable"
fi

echo "$NR_SAMPLES" > "$DEBUGFS/nr_samples"
echo "$PERCENTILE" > "$DEBUGFS/nth_percentile"
echo 1 > "$DEBUGFS/benchmark"

disable_tracepoints

read_val() {
	cat "$DEBUGFS/$1/$2"
}

STATS="irq preempt irq_save"
FIELDS="median average percentile"

UNIT="(cycles)"

# Compute first column width from the widest label
COL1=${#KVER}
for s in $ARCH $STATS; do
	[ ${#s} -gt "$COL1" ] && COL1=${#s}
done
COL1=$((COL1 + 2))

printf "%-${COL1}s" "$KVER"
for field in $FIELDS; do
    if [ "$field" == "percentile" ]; then
        printf "%15s" "$PERCENTILE-$field"
    else
	    printf "%15s" "$field"
	fi
done
printf "\n"

printf "%-${COL1}s" "$ARCH"
for field in $FIELDS; do
	printf "%15s" "$UNIT"
done
printf "\n"

printf "%-${COL1}s" "--------"
for field in $FIELDS; do
	printf "%15s" "--------"
done
printf "\n"

for stat in $STATS; do
	printf "%-${COL1}s" "$stat"
	for field in $FIELDS; do
		printf "%15s" "$(read_val "$stat" "$field")"
	done
	printf "\n"
done
