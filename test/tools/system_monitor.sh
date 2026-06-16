#!/bin/bash
# system_monitor.sh — Crash diagnosis monitor (disk-persistent)
#
# Writes to disk with sync after every sample. Survives hard resets.
# Also dumps full dmesg periodically so kernel messages are preserved.
#
# Usage: sudo bash scripts/system_monitor.sh [interval_secs] [log_dir]

INTERVAL=${1:-10}
LOGDIR=${2:-"/home/busmust/socketcan/socketcan/test/results/diag"}

mkdir -p "$LOGDIR"
MONITOR_LOG="$LOGDIR/system_monitor.log"
DMESG_LOG="$LOGDIR/dmesg_snapshot.log"

DMESG_BASE=$(sudo dmesg | wc -l)

{
    echo "=== System Monitor Started: $(date '+%Y-%m-%d %H:%M:%S') ==="
    echo "Interval: ${INTERVAL}s"
    echo "Kernel: $(uname -r)"
    echo "Memory: $(free -m | awk '/Mem:/{printf "%dMB total", $2}')"
    echo "CPU temp governor: $(cat /sys/class/thermal/thermal_zone0/type 2>/dev/null || echo N/A)"
    echo ""
} > "$MONITOR_LOG"

# Save full dmesg at start
sudo dmesg > "$DMESG_LOG"
sync

dmesg_prev=$DMESG_BASE
iter=0

while true; do
    iter=$((iter + 1))
    ts=$(date '+%Y-%m-%d %H:%M:%S')
    uptime_s=$(cat /proc/uptime | awk '{printf "%.0f", $1}')

    # Memory
    mem_line=$(free -m | awk '/Mem:/{printf "total=%d used=%d free=%d avail=%d", $2, $3, $4, $7}')
    swap_line=$(free -m | awk '/Swap:/{printf "swap_used=%d", $3}')
    slab_info=$(cat /proc/meminfo | awk '/SUnreclaim:/{printf "slab_unreclaim=%dkB", $2}')

    # Temperature
    temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo "N/A")
    [ "$temp" != "N/A" ] && temp="$(echo "scale=1; $temp / 1000" | bc)C"

    # CAN interfaces
    can_count=$(ip -o link show 2>/dev/null | grep -c ': can[0-9]*:')
    can_up=$(ip -o link show 2>/dev/null | grep ': can[0-9]*:' | grep -c 'UP' || echo 0)

    # USB devices
    usb_count=$(lsusb 2>/dev/null | grep -ci '0810:' || echo 0)

    # cangen processes
    cangen_count=$(pgrep -c cangen 2>/dev/null || echo 0)

    # OOM check
    oom_count=$(sudo dmesg | grep -c 'Out of memory\|oom-kill\|Killed process' 2>/dev/null || echo 0)

    # New dmesg messages
    dmesg_now=$(sudo dmesg | wc -l)
    new_msgs=""
    if [ "$dmesg_now" -gt "$dmesg_prev" ]; then
        new_msgs=$(sudo dmesg | tail -n +$((dmesg_prev + 1)) | grep -iE 'bmcan|usb|xhci|error|warn|fail|oom|kill|panic|hung|lockup|alloc.*fail|unable|deadlock|stall' 2>/dev/null)
    fi
    dmesg_prev=$dmesg_now

    # Write log line
    echo "[$ts] up=${uptime_s}s $mem_line $swap_line $slab_info temp=$temp can=$can_count(up=$can_up) usb=$usb_count cangen=$cangen_count oom=$oom_count" >> "$MONITOR_LOG"

    # Write new dmesg if any
    if [ -n "$new_msgs" ]; then
        echo "[$ts] dmesg:" >> "$MONITOR_LOG"
        echo "$new_msgs" | sed "s/^/  /" >> "$MONITOR_LOG"
    fi

    # Every 6 iterations (~1 min), log detail + sync
    if [ $((iter % 6)) -eq 0 ]; then
        # CAN error counters
        for iface in $(ip -o link show 2>/dev/null | grep ': can[0-9]*:' | awk -F: '{print $2}' | tr -d ' '); do
            details=$(ip -details link show $iface 2>/dev/null)
            state=$(echo "$details" | grep -oP 'state \K\S+' || echo "?")
            bus_err=$(echo "$details" | grep -oP '\(bus-errors: \K\d+' || echo "0")
            echo "  $iface: state=$state bus_err=$bus_err" >> "$MONITOR_LOG"
        done

        # Top memory consumers
        ps aux --sort=-%mem | head -5 | awk '{printf "  top_mem: %s pid=%s rss=%.1fMB %s\n", $4"%", $2, $6/1024, $11}' >> "$MONITOR_LOG"

        # Sync to disk
        sync
    fi

    # Every 60 iterations (~10 min), full dmesg snapshot
    if [ $((iter % 60)) -eq 0 ]; then
        sudo dmesg > "$DMESG_LOG"
        sync
    fi

    sleep $INTERVAL
done
