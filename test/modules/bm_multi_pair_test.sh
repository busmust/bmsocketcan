#!/bin/bash
# Module: Multi-Pair Communication Test
#
# Tests concurrent TX/RX on multiple CAN channel pairs simultaneously.
# Uses all available cross-device pairs from hardware_config.conf.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

MP_BITRATE=500000
MP_DBITRATE=2000000
MP_DURATION=3

# Shared result
MP_TOTAL_TX=0
MP_TOTAL_RX=0

################################################################################
# Helpers
################################################################################

setup_pair() {
    local tx=$1 rx=$2
    sudo ip link set $tx down 2>/dev/null
    sudo ip link set $rx down 2>/dev/null
    sleep 0.3
    sudo ip link set $tx up type can bitrate $MP_BITRATE dbitrate $MP_DBITRATE \
        fd on termination 120 2>/dev/null || return 1
    sudo ip link set $rx up type can bitrate $MP_BITRATE dbitrate $MP_DBITRATE \
        fd on termination 120 2>/dev/null || return 1
    sleep 1
    return 0
}

# Test single pair: send frames and verify RX via kernel counter (avoids candump timing issues)
test_pair_basic() {
    local tx=$1 rx=$2
    log_info "  Testing pair $tx -> $rx"

    local attempt
    for attempt in 1 2 3; do
        local rx_before=$(cat /sys/class/net/$rx/statistics/rx_packets 2>/dev/null || echo 0)
        sudo cansend $tx 1FF#AABBCCDD >/dev/null 2>&1
        sleep 0.5
        local rx_after=$(cat /sys/class/net/$rx/statistics/rx_packets 2>/dev/null || echo 0)
        local delta=$((rx_after - rx_before))

        if [ "$delta" -gt 0 ]; then
            log_success "  $tx -> $rx: OK"
            return 0
        fi
        [ "$attempt" -lt 3 ] && sleep 0.5
    done

    log_fail "  $tx -> $rx: no frames received after 3 attempts"
    return 1
}

# Test all pairs concurrently with burst traffic
test_concurrent_burst() {
    local pairs=("$@")
    log_info "  Concurrent burst test on ${#pairs[@]} pairs (${MP_DURATION}s)"

    local rx_before=()
    local pids=()

    # Snapshot RX counters
    local i=0
    for pair in "${pairs[@]}"; do
        local rx=${pair#*:}
        rx_before+=("$(cat /sys/class/net/$rx/statistics/rx_packets 2>/dev/null || echo "0")")
        ((i++))
    done

    # Launch cangen on all TX ports simultaneously
    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        cangen $tx -g 0 -f -b -i -L 8 -n 999999 >/dev/null 2>&1 &
        pids+=($!)
    done

    sleep $MP_DURATION

    for pid in "${pids[@]}"; do
        kill $pid 2>/dev/null
        wait $pid 2>/dev/null
    done

    sleep 1

    # Check results
    local all_ok=1
    i=0
    MP_TOTAL_TX=0
    MP_TOTAL_RX=0

    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        local rx=${pair#*:}
        local rx_after=$(cat /sys/class/net/$rx/statistics/rx_packets 2>/dev/null || echo "0")
        local recv=$((rx_after - rx_before[$i]))
        local fps=$((recv / MP_DURATION))
        MP_TOTAL_RX=$((MP_TOTAL_RX + recv))
        log_info "    $tx -> $rx: $recv frames (${fps} fps)"

        if [ "$recv" -le 0 ]; then
            log_fail "    $tx -> $rx: no frames!"
            all_ok=0
        fi

        # Check bus state
        if ip -details link show $rx 2>/dev/null | grep -q "BUS-OFF"; then
            log_fail "    $rx: BUS-OFF!"
            all_ok=0
        fi
        ((i++))
    done

    local total_fps=$((MP_TOTAL_RX / MP_DURATION))
    log_info "  Total: $MP_TOTAL_RX frames across ${#pairs[@]} pairs (${total_fps} fps aggregate)"

    if [ "$all_ok" -eq 1 ]; then
        log_success "  Concurrent burst test passed"
        return 0
    else
        log_fail "  Concurrent burst test failed on one or more pairs"
        return 1
    fi
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Multi-Pair Communication Test"
    log_info "=========================================="
    echo ""

    if ! command -v cangen >/dev/null 2>&1; then
        log_error "cangen not found"
        start_testcase "MultiPair_Prerequisite"
        test_fail "cangen not available"
        return 1
    fi

    # Load hardware config
    local hwconf="$SCRIPT_DIR/../hardware_config.conf"
    if [ ! -f "$hwconf" ]; then
        log_error "hardware_config.conf not found"
        return 1
    fi
    source "$hwconf"

    local pairs=($HW_CONN_PAIRS)
    if [ ${#pairs[@]} -lt 1 ]; then
        log_error "No connection pairs defined in hardware config"
        return 1
    fi

    log_info "Pairs: ${pairs[*]}"
    log_info "Config: CAN FD ${MP_BITRATE}/${MP_DBITRATE}"
    echo ""

    # Backup all port configs
    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        local rx=${pair#*:}
        backup_port_config $tx 2>/dev/null
        backup_port_config $rx 2>/dev/null
    done

    # Setup all pairs
    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        local rx=${pair#*:}
        if ! setup_pair $tx $rx; then
            log_error "Failed to setup pair $tx/$rx"
            continue
        fi
    done
    sleep 1

    # Warmup: send+receive on each pair to prime the CAN path
    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        local rx=${pair#*:}
        timeout 2 candump $rx -n 1 -t A &>/dev/null &
        sleep 0.2
        sudo cansend $tx 0FF#AABBCCDD >/dev/null 2>&1 || true
        wait 2>/dev/null || true
    done
    sleep 0.5

    # Test 1: Basic pair verification
    local pair_ok=1
    start_testcase "MultiPair_Basic"
    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        local rx=${pair#*:}
        if ! test_pair_basic $tx $rx; then
            pair_ok=0
        fi
    done
    if [ "$pair_ok" -eq 1 ]; then
        test_pass "All pairs verified"
    else
        test_fail "One or more pairs failed basic test"
    fi
    echo ""

    # Test 2: Concurrent burst
    start_testcase "MultiPair_Concurrent"
    if test_concurrent_burst "${pairs[@]}"; then
        test_pass "Concurrent burst: $MP_TOTAL_RX frames"
    else
        test_fail "Concurrent burst failed"
    fi
    echo ""

    # Restore configs
    log_info "Restoring port configuration..."
    for pair in "${pairs[@]}"; do
        local tx=${pair%%:*}
        local rx=${pair#*:}
        restore_port_config $tx 2>/dev/null
        restore_port_config $rx 2>/dev/null
    done
    verify_port_config ${pairs[0]%%:*}

    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
