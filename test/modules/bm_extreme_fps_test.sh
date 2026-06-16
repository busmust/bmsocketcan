#!/bin/bash
# Module: Extreme Frame Rate Performance Test
#
# Measures driver throughput at maximum frame rate.
# Configuration: CAN FD 1M/8M, standard ID, BRS, FDF, 8-byte payload.
# Target: TX and RX both >= 20K fps
#
# Method: TX uses 8x parallel cangen to saturate USB/bus.
#         RX uses 1x cangen to avoid CAN bus overload (multi-process TX causes
#         frame loss at the physical layer, underestimating RX throughput).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

# Port configuration
EXT_BITRATE=1000000
EXT_DBITRATE=8000000

# Test duration (seconds)
TEST_DURATION=5

# Number of parallel cangen processes
# 1x avoids CAN bus overload (multi-process causes frame loss at physical layer,
# underestimating RX throughput). 1x cangen already reaches 200K+ TX / 21K+ RX.
PARALLEL_TX_PROCS=1
PARALLEL_RX_PROCS=1

# FPS threshold (override via env var)
TARGET_FPS=${EXT_TARGET_FPS:-20000}

# Shared result variables
EXT_TX_FPS=0
EXT_RX_FPS=0

################################################################################
# Port Setup
################################################################################

setup_extreme_ports() {
    local tx=$1 rx=$2
    log_info "  Configuring ${tx}/${rx} -> ${EXT_BITRATE}/${EXT_DBITRATE} CAN FD"

    sudo ip link set $tx down 2>/dev/null
    sudo ip link set $rx down 2>/dev/null
    sleep 0.5

    sudo ip link set $tx up type can bitrate $EXT_BITRATE dbitrate $EXT_DBITRATE \
        fd on termination 120 2>/dev/null || return 1
    sudo ip link set $rx up type can bitrate $EXT_BITRATE dbitrate $EXT_DBITRATE \
        fd on termination 120 2>/dev/null || return 1
    sleep 1
    return 0
}

# Reset RX interface statistics by cycling the interface
reset_rx_counters() {
    local rx=$1
    sudo ip link set $rx down 2>/dev/null
    sleep 0.3
    sudo ip link set $rx up type can bitrate $EXT_BITRATE dbitrate $EXT_DBITRATE \
        fd on termination 120 2>/dev/null
    sleep 0.3
}

################################################################################
# Multi-process TX helper
################################################################################

# Launch N parallel cangen processes, wait for duration, then kill them all
run_parallel_cangen() {
    local dev=$1
    local duration=$2
    local procs=$3
    local pids=()

    for ((j = 0; j < procs; j++)); do
        cangen $dev -g 0 -f -b -i -L 8 -n 999999 >/dev/null 2>&1 &
        pids+=($!)
    done

    sleep $duration

    for pid in "${pids[@]}"; do
        kill $pid 2>/dev/null
        wait $pid 2>/dev/null
    done
}

################################################################################
# TX Test
################################################################################

test_tx_fps() {
    local tx=$1
    local tx_before tx_after fps

    tx_before=$(cat /sys/class/net/$tx/statistics/tx_packets 2>/dev/null || echo "0")

    run_parallel_cangen $tx $TEST_DURATION $PARALLEL_TX_PROCS

    tx_after=$(cat /sys/class/net/$tx/statistics/tx_packets 2>/dev/null || echo "0")
    local sent=$((tx_after - tx_before))

    if [ "$sent" -le 0 ]; then
        log_error "  TX: no frames sent"
        EXT_TX_FPS=0
        return 1
    fi

    fps=$(awk "BEGIN {printf \"%.0f\", $sent / $TEST_DURATION}")
    EXT_TX_FPS=$fps

    log_info "  TX: $sent frames in ${TEST_DURATION}s = ${fps} fps (${PARALLEL_TX_PROCS}x cangen)"

    if [ "$fps" -ge "$TARGET_FPS" ]; then
        log_success "  TX: ${fps} fps >= ${TARGET_FPS} [PASS]"
        return 0
    else
        log_fail "  TX: ${fps} fps < ${TARGET_FPS} [FAIL]"
        return 1
    fi
}

################################################################################
# RX Test
################################################################################

test_rx_fps() {
    local tx=$1 rx=$2
    local rx_before rx_after fps

    # Reset RX counters
    reset_rx_counters $rx
    sleep 0.3

    rx_before=$(cat /sys/class/net/$rx/statistics/rx_packets 2>/dev/null || echo "0")

    run_parallel_cangen $tx $TEST_DURATION $PARALLEL_RX_PROCS

    # Wait for remaining frames to propagate
    sleep 1

    rx_after=$(cat /sys/class/net/$rx/statistics/rx_packets 2>/dev/null || echo "0")
    local recv=$((rx_after - rx_before))

    if [ "$recv" -le 0 ]; then
        log_error "  RX: no frames received"
        EXT_RX_FPS=0
        return 1
    fi

    fps=$(awk "BEGIN {printf \"%.0f\", $recv / $TEST_DURATION}")
    EXT_RX_FPS=$fps

    log_info "  RX: $recv frames in ${TEST_DURATION}s = ${fps} fps (${PARALLEL_RX_PROCS}x cangen)"

    if [ "$fps" -ge "$TARGET_FPS" ]; then
        log_success "  RX: ${fps} fps >= ${TARGET_FPS} [PASS]"
        return 0
    else
        log_fail "  RX: ${fps} fps < ${TARGET_FPS} [FAIL]"
        return 1
    fi
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Extreme Frame Rate Test"
    log_info "=========================================="
    echo ""

    # Gen2.5 devices (GD32 + MCP2518FD via SPI) have hardware-limited throughput.
    # Extreme FPS test requires Gen3 (STM32H7 + internal CAN FD controller).
    if ! is_gen3_device; then
        log_warn "Non-Gen3 device detected - skipping extreme FPS test (Gen3 only)"
        return 0
    fi

    # Check dependencies
    if ! command -v cangen >/dev/null 2>&1; then
        log_error "cangen not found (install: sudo apt install can-utils)"
        start_testcase "Extreme_FPS_Prerequisite"
        test_fail "cangen not available"
        return 1
    fi

    # Read ports
    if [ ! -f "$RESULTS_DIR/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi
    local ports
    ports=($(cat "$RESULTS_DIR/connected_ports.txt"))
    local tx=${ports[0]} rx=${ports[1]}

    log_info "TX port: $tx"
    log_info "RX port: $rx"
    log_info "Config:  CAN FD ${EXT_BITRATE}/${EXT_DBITRATE}, std ID, BRS, 8-byte"
    log_info "Workers: TX ${PARALLEL_TX_PROCS}x, RX ${PARALLEL_RX_PROCS}x parallel cangen"
    log_info "Target:  >= ${TARGET_FPS} fps"
    echo ""

    # Backup and reconfigure ports
    backup_port_config $tx
    backup_port_config $rx

    if ! setup_extreme_ports $tx $rx; then
        log_error "Failed to configure ports for extreme FPS test"
        restore_port_config $tx
        restore_port_config $rx
        start_testcase "Extreme_FPS_Port_Setup"
        test_fail "Port configuration failed"
        return 1
    fi

    # Warmup
    sudo cansend $tx 123##10102030405060708 >/dev/null 2>&1
    sleep 0.5

    # Test 1: TX Frame Rate
    start_testcase "Extreme_FPS_TX"
    if test_tx_fps $tx; then
        test_pass "TX: ${EXT_TX_FPS} fps"
    else
        test_fail "TX: ${EXT_TX_FPS} fps < ${TARGET_FPS}"
    fi
    echo ""
    sleep 1

    # Test 2: RX Frame Rate (TX -> CAN bus -> RX)
    start_testcase "Extreme_FPS_RX"
    if test_rx_fps $tx $rx; then
        test_pass "RX: ${EXT_RX_FPS} fps"
    else
        test_fail "RX: ${EXT_RX_FPS} fps < ${TARGET_FPS}"
    fi
    echo ""

    # Restore original port configuration
    log_info "Restoring port configuration..."
    restore_port_config $tx
    restore_port_config $rx
    verify_port_config $tx

    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
