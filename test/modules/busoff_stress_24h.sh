#!/bin/bash
# Bus-Off Stress Test — 24-Hour Long Run (Multi-Pair, BMAPI Pattern)
#
# Each pair runs independently in parallel:
#   DUT: 4 IDs at 10/20/40/40ms via cangen (BM_Send)
#   Partner: 1ms CAN FD 64B heavy TX via bmcan_api txtask (BM_SetTxTask)
#
# 4 channel pairs: can0↔can6, can1↔can7, can2↔can4, can3↔can5
#
# Usage:
#   sudo bash test/modules/busoff_stress_24h.sh          # 24 hours
#   sudo bash test/modules/busoff_stress_24h.sh 4        # 4 hours
#   sudo bash test/modules/busoff_stress_24h.sh 0 60     # 60 cycles exact

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"

HOURS=${1:-24}
EXACT_CYCLES=${2:-0}

if [ "$EXACT_CYCLES" -gt 0 ]; then
    CYCLES=$EXACT_CYCLES
else
    CYCLES=$(echo "$HOURS * 3600 / 1.05" | bc | cut -d. -f1)
fi

TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
LOGDIR="$RESULTS_DIR/busoff_stress_24h_${TIMESTAMP}"
mkdir -p "$LOGDIR"

# Port pairs (hardware wiring)
PAIRS=("can0:can6" "can1:can7" "can2:can4" "can3:can5")
NUM_PAIRS=${#PAIRS[@]}

log_info "=========================================="
log_info "Bus-Off Stress Test — Multi-Pair Long Run"
log_info "=========================================="
log_info "Duration: ${HOURS}h (~${CYCLES} cycles per pair)"
log_info "Pattern: BMAPI txtask (DUT 4-ID + Partner 1ms/64B)"
log_info "Pairs: ${PAIRS[*]}"
log_info "Logs: $LOGDIR"
echo ""

# Prepare environment
sudo killall cangen candump 2>/dev/null || true
for p in $(ls /sys/class/net/ 2>/dev/null | grep can); do
    sudo ip link set $p down 2>/dev/null || true
done
sleep 0.5

if lsmod | grep -q bmcan; then
    sudo rmmod bmcan 2>/dev/null || true
    sleep 0.5
fi
sudo insmod "$SCRIPT_DIR/../../out/bmcan.ko"
sleep 1

log_info "Starting at $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

################################################################################
# Single-pair worker: runs independently for one DUT/Partner pair
################################################################################
run_pair_worker() {
    local dut=$1 partner=$2 cycles=$3 logfile=$4
    local ch_num=$(echo $dut | sed 's/can//')

    {
        echo "=== Pair $dut <-> $partner: $cycles cycles ==="

        # DUT up (firmware handles bus-off recovery autonomously)
        sudo ip link set $dut down 2>/dev/null || true
        sleep 0.2
        sudo ip link set $dut up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
        sleep 0.5

        # Partner up first, then start DUT txtask
        sudo ip link set $partner down 2>/dev/null || true
        sleep 0.2
        sudo ip link set $partner up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
        sleep 0.3

        # DUT: software TX via cangen (matching BM_Send from bmapi_transmit_only)
        # 4 CAN IDs at 10/20/40/40ms
        CG_PIDS=()
        sudo cangen $dut -g 10 -L 8 -I 117 -D 0300000000000000 >/dev/null 2>&1 &
        CG_PIDS+=($!)
        sudo cangen $dut -g 20 -L 8 -I 1B5 -D 0300000000000000 >/dev/null 2>&1 &
        CG_PIDS+=($!)
        sudo cangen $dut -g 40 -L 8 -I 224 -D 0300000000000000 >/dev/null 2>&1 &
        CG_PIDS+=($!)
        sudo cangen $dut -g 40 -L 8 -I 298 -D 0300000000000000 >/dev/null 2>&1 &
        CG_PIDS+=($!)

        # Verify DUT transmitting
        local rx_before=$(cat /sys/class/net/$partner/statistics/rx_packets 2>/dev/null || echo 0)
        sleep 2
        local rx_after=$(cat /sys/class/net/$partner/statistics/rx_packets 2>/dev/null || echo 0)
        if [ $((rx_after - rx_before)) -eq 0 ]; then
            echo "  DUT $dut not transmitting — aborting pair"
            return 1
        fi
        echo "  DUT $dut verified: $((rx_after - rx_before)) frames"

        # Close partner — DUT enters bus-off
        sudo ip link set $partner down 2>/dev/null || true
        sleep 0.5

        local recovered=0 failed=0 consecutive=0

        for i in $(seq 1 $cycles); do
            # Partner open
            local open_ok=false
            local retry
            for retry in 1 2 3; do
                sudo ip link set $partner down 2>/dev/null || true
                sleep 0.2
                sudo ip link set $partner up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
                sleep 0.3
                if ip link show $partner 2>/dev/null | grep -q "UP"; then
                    open_ok=true
                    break
                fi
                sleep 0.3
            done

            if [ "$open_ok" = false ]; then
                failed=$((failed + 1))
                consecutive=$((consecutive + 1))
                continue
            fi

            # Partner heavy TX: 1ms CAN FD 64B
            sudo $API_BIN txtasks --index 0 --type fixed --id $((0x500 + ch_num)) --cycle 1 \
                --length 64 --payload AA --fd --brs --device $partner >/dev/null 2>&1 || true

            # Poll for DUT frames (1s dwell)
            local logf="/tmp/busoff_pair_$$_${partner}.log"
            rm -f "$logf"
            timeout 2 candump $partner -n 100 -t A > "$logf" 2>/dev/null || true
            local rx_count
            rx_count=$(grep -cE ' 117 | 1B5 | 224 | 298 ' "$logf" 2>/dev/null) || rx_count=0
            rm -f "$logf"

            # Partner close
            sudo $API_BIN invalidate --txtask --tx-range 0-0 --device $partner >/dev/null 2>&1 || true
            sudo ip link set $partner down 2>/dev/null || true

            if [ "$rx_count" -gt 0 ]; then
                recovered=$((recovered + 1))
                consecutive=0
            else
                failed=$((failed + 1))
                consecutive=$((consecutive + 1))
            fi

            # Progress log every 100 cycles or 1h
            if [ $((i % 100)) -eq 0 ]; then
                local total=$((recovered + failed))
                local rate=0
                [ "$total" -gt 0 ] && rate=$((recovered * 100 / total))
                echo "  $dut: $i/$cycles (ok=$recovered fail=$failed rate=${rate}%)"
            fi

            if [ "$consecutive" -ge 20 ]; then
                echo "  $dut: Bailing after 20 consecutive failures at cycle $i"
                break
            fi
        done

        # Cleanup
        kill ${CG_PIDS[@]} 2>/dev/null || true
        sudo ip link set $dut down 2>/dev/null || true
        sudo ip link set $partner down 2>/dev/null || true

        local total=$((recovered + failed))
        local rate=0
        [ "$total" -gt 0 ] && rate=$((recovered * 100 / total))
        echo ""
        echo "=== Pair $dut <-> $partner Final Stats ==="
        echo "  Cycles: $total"
        echo "  Recovered: $recovered"
        echo "  Failed: $failed"
        echo "  Rate: ${rate}%"
    } > "$logfile" 2>&1
}

################################################################################
# Launch all pair workers in parallel
################################################################################

PIDS=()
for pair in "${PAIRS[@]}"; do
    dut=$(echo $pair | cut -d: -f1)
    partner=$(echo $pair | cut -d: -f2)
    logfile="$LOGDIR/${dut}_${partner}.log"

    log_info "Starting pair $dut <-> $partner ($CYCLES cycles)"
    run_pair_worker $dut $partner $CYCLES "$logfile" &
    PIDS+=($!)
    sleep 0.2  # stagger starts slightly
done

echo ""
log_info "All ${#PIDS[@]} pair workers running (PIDs: ${PIDS[*]})"
log_info "Monitor: tail -f $LOGDIR/*.log"

# Wait for all workers
for pid in "${PIDS[@]}"; do
    wait $pid 2>/dev/null || true
done

################################################################################
# Aggregate results
################################################################################

echo ""
log_info "=========================================="
log_info "Aggregate Results"
log_info "=========================================="

TOTAL_CYCLES=0
TOTAL_RECOVERED=0
TOTAL_FAILED=0

for pair in "${PAIRS[@]}"; do
    dut=$(echo $pair | cut -d: -f1)
    partner=$(echo $pair | cut -d: -f2)
    logfile="$LOGDIR/${dut}_${partner}.log"

    pair_total=$(grep -oP 'Cycles: \K\d+' "$logfile" 2>/dev/null || echo 0)
    pair_ok=$(grep -oP 'Recovered: \K\d+' "$logfile" 2>/dev/null || echo 0)
    pair_fail=$(grep -oP 'Failed: \K\d+' "$logfile" 2>/dev/null || echo 0)
    pair_rate=$(grep -oP 'Rate: \K\d+' "$logfile" 2>/dev/null || echo 0)

    TOTAL_CYCLES=$((TOTAL_CYCLES + pair_total))
    TOTAL_RECOVERED=$((TOTAL_RECOVERED + pair_ok))
    TOTAL_FAILED=$((TOTAL_FAILED + pair_fail))

    log_info "  $dut<->$partner: $pair_ok/$pair_total ($pair_rate%)"
done

agg_rate=0
[ "$TOTAL_CYCLES" -gt 0 ] && agg_rate=$((TOTAL_RECOVERED * 100 / TOTAL_CYCLES))

echo ""
log_info "  Total: $TOTAL_RECOVERED/$TOTAL_CYCLES ($agg_rate%)"

# Post-stress health
final_count=$(ip -o link show 2>/dev/null | grep -c ': can[0-9]*:')
oops_count=$(sudo dmesg | grep -cE 'Oops|BUG:|panic|Call trace' 2>/dev/null) || oops_count=0
log_info "  Interfaces: $final_count (no leak: $([ $final_count -eq 8 ] && echo yes || echo NO))"
log_info "  Kernel: $([ $oops_count -eq 0 ] && echo stable || echo OOPS=$oops_count)"

echo ""
log_info "Finished at $(date '+%Y-%m-%d %H:%M:%S')"
log_info "Logs: $LOGDIR"
