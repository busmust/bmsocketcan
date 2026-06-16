#!/bin/bash
# Module 4: Basic Communication Test
# Tests standard CAN and CAN-FD frame TX/RX between cross-channel ports

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# Test Functions
################################################################################

test_standard_frame() {
    local tx=$1
    local rx=$2
    log_info "Testing standard CAN frame: $tx -> $rx"

    local logf="/tmp/std_test_$$.log"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1

    sudo cansend $tx 123#DEADBEEF >/dev/null 2>&1
    wait $pid 2>/dev/null || true

    local received; received=$(grep -c "123" "$logf" 2>/dev/null) || received=0
    [[ "$received" =~ ^[0-9]+$ ]] || received=0
    rm -f "$logf"

    if [ "$received" -gt 0 ]; then
        log_success "Standard frame: $received frame(s) received"
        return 0
    else
        log_fail "Standard frame: no frames received"
        return 1
    fi
}

test_fd_frame() {
    local tx=$1
    local rx=$2
    log_info "Testing CAN-FD frame: $tx -> $rx"

    local logf="/tmp/canfd_test_$$.log"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1

    sudo cansend $tx 123##1DEADBEEF >/dev/null 2>&1
    wait $pid 2>/dev/null || true

    local received; received=$(grep -c "123" "$logf" 2>/dev/null) || received=0
    [[ "$received" =~ ^[0-9]+$ ]] || received=0
    rm -f "$logf"

    if [ "$received" -gt 0 ]; then
        log_success "CAN-FD frame: $received frame(s) received"
        return 0
    else
        log_fail "CAN-FD frame: no frames received"
        return 1
    fi
}

test_bidirectional() {
    local port_a=$1
    local port_b=$2
    log_info "Testing bidirectional communication"

    # Listen on both ports with -n 2 (one frame each direction)
    local logf_b="/tmp/bidir_b_$$.log"
    local logf_a="/tmp/bidir_a_$$.log"
    timeout 2 candump $port_b -n 2 -t A 2>/dev/null > "$logf_b" &
    local pid_b=$!
    timeout 2 candump $port_a -n 2 -t A 2>/dev/null > "$logf_a" &
    local pid_a=$!
    sleep 0.1

    # Send bidirectional
    sudo cansend $port_a 100#DEADBEEF >/dev/null 2>&1
    sudo cansend $port_b 200#AABBCCDD >/dev/null 2>&1
    wait $pid_a $pid_b 2>/dev/null || true

    local result1; result1=$(grep -c "100" "$logf_b" 2>/dev/null) || result1=0
    local result2; result2=$(grep -c "200" "$logf_a" 2>/dev/null) || result2=0
    [[ "$result1" =~ ^[0-9]+$ ]] || result1=0
    [[ "$result2" =~ ^[0-9]+$ ]] || result2=0

    rm -f "$logf_b" "$logf_a"

    if [ "$result1" -gt 0 ] && [ "$result2" -gt 0 ]; then
        log_success "Bidirectional: A->B ($result1 frames) and B->A ($result2 frames) both work"
        return 0
    else
        log_fail "Bidirectional: one direction failed (A->B: $result1, B->A: $result2)"
        return 1
    fi
}

test_error_counter() {
    local ports=("$@")
    log_info "Checking error counters"

    for port in "${ports[@]}"; do
        local tx_err=$(ip -details link show $port | grep "berr-counter" | awk '{print $2}')
        local rx_err=$(ip -details link show $port | grep "berr-counter" | awk '{print $3}')

        tx_err=${tx_err:-0}
        rx_err=${rx_err:-0}
        [[ "$tx_err" =~ ^[0-9]+$ ]] || tx_err=0
        [[ "$rx_err" =~ ^[0-9]+$ ]] || rx_err=0

        if [ "$tx_err" -gt 127 ] || [ "$rx_err" -gt 127 ]; then
            log_fail "$port: high error count (tx:$tx_err, rx:$rx_err)"
            return 1
        fi

        log_info "$port: errors OK (tx:$tx_err, rx:$rx_err)"
    done

    log_success "Error counters normal"
    return 0
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module 4: Basic Communication Test"
    log_info "=========================================="
    echo ""

    # Read ports
    if [ ! -f "$RESULTS_DIR/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi

    local ports=($(cat "$RESULTS_DIR/connected_ports.txt"))

    if [ ${#ports[@]} -lt 2 ]; then
        log_error "Need at least 2 connected ports (found: ${#ports[@]})"
        return 1
    fi

    local tx=${ports[0]}
    local rx=${ports[1]}

    log_info "TX port: $tx"
    log_info "RX port: $rx"
    echo ""

    # Test standard CAN frame
    start_testcase "Basic_Standard_Frame"
    log_info "Sending from $tx to $rx..."
    if test_standard_frame "$tx" "$rx"; then
        test_pass "Standard CAN frame TX/RX OK"
    else
        test_fail "Standard CAN frame TX/RX failed"
    fi
    echo ""

    # Test CAN-FD frame
    start_testcase "Basic_FD_Frame"
    log_info "Sending FD frame from $tx to $rx..."
    if test_fd_frame "$tx" "$rx"; then
        test_pass "CAN-FD frame TX/RX OK"
    else
        test_fail "CAN-FD frame TX/RX failed"
    fi
    echo ""

    # Test bidirectional
    start_testcase "Basic_Bidirectional"
    if test_bidirectional "$tx" "$rx"; then
        test_pass "Bidirectional communication OK"
    else
        test_fail "Bidirectional communication failed"
    fi
    echo ""

    # Test error counters
    start_testcase "Basic_Error_Counters"
    if test_error_counter "${ports[@]}"; then
        test_pass "Error counters OK"
    else
        test_fail "Error counters abnormal"
    fi
    echo ""

    log_success "Basic communication test completed"
    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    source "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"
    RESULTS_DIR="$(dirname "${BASH_SOURCE[0]}")/../results"
    main "$@"
fi
