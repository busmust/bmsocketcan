#!/bin/bash
# Module: Mode Test
#
# Tests CAN interface mode switching: normal, loopback, listen-only.
# Verifies each mode via ip-link and confirms frame behavior matches expected mode.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

MODE_BITRATE=500000
MODE_DBITRATE=2000000

################################################################################
# Helpers
################################################################################

# Bring port up in specified mode
set_mode() {
    local port=$1 mode=$2
    sudo ip link set $port down 2>/dev/null
    sleep 0.3
    case "$mode" in
        normal)
            sudo ip link set $port up type can bitrate $MODE_BITRATE \
                dbitrate $MODE_DBITRATE fd on loopback off listen-only off \
                termination 120 2>/dev/null
            ;;
        loopback)
            sudo ip link set $port up type can bitrate $MODE_BITRATE \
                dbitrate $MODE_DBITRATE fd on loopback on termination 120 2>/dev/null
            ;;
        listen-only)
            sudo ip link set $port up type can bitrate $MODE_BITRATE \
                dbitrate $MODE_DBITRATE fd on listen-only on termination 120 2>/dev/null
            ;;
    esac
    sleep 0.5
}

# Verify mode via ip -details link show
check_mode() {
    local port=$1 mode=$2
    local details
    details=$(ip -details link show $port 2>/dev/null)
    case "$mode" in
        normal)
            echo "$details" | grep -q "LOOPBACK" && return 1
            echo "$details" | grep -q "LISTEN-ONLY" && return 1
            ;;
        loopback)
            echo "$details" | grep -q "LOOPBACK" || return 1
            ;;
        listen-only)
            echo "$details" | grep -q "LISTEN-ONLY" || return 1
            ;;
    esac
    return 0
}

################################################################################
# Test Cases
################################################################################

test_normal_mode() {
    local tx=$1 rx=$2
    log_info "  Testing normal mode on $tx"

    set_mode $tx normal || { log_fail "  Failed to set normal mode"; return 1; }
    set_mode $rx normal || { log_fail "  Failed to set RX normal mode"; return 1; }

    if ! check_mode $tx normal; then
        log_fail "  Mode flag check failed for normal"
        return 1
    fi

    # Verify communication works in normal mode
    local logf="/tmp/mode_normal_$$.log"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1
    sudo cansend $tx 123#AABBCCDD >/dev/null 2>&1
    wait $pid 2>/dev/null || true

    local recv; recv=$(grep -c "123" "$logf" 2>/dev/null) || recv=0
    rm -f "$logf"

    if [ "$recv" -gt 0 ]; then
        log_success "  Normal mode: TX/RX works ($recv frames)"
        return 0
    else
        log_fail "  Normal mode: no frames received"
        return 1
    fi
}

test_loopback_mode() {
    local port=$1
    log_info "  Testing loopback mode on $port"

    set_mode $port loopback || { log_fail "  Failed to set loopback mode"; return 1; }

    if ! check_mode $port loopback; then
        log_fail "  Mode flag check failed for loopback"
        return 1
    fi

    # Loopback: frame should be received on same port
    local logf="/tmp/mode_loopback_$$.log"
    timeout 2 candump $port -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1
    sudo cansend $port 456#11223344 >/dev/null 2>&1
    wait $pid 2>/dev/null || true

    local recv; recv=$(grep -c "456" "$logf" 2>/dev/null) || recv=0
    rm -f "$logf"

    if [ "$recv" -gt 0 ]; then
        log_success "  Loopback mode: self-reception works ($recv frames)"
        return 0
    else
        log_fail "  Loopback mode: no self-reception"
        return 1
    fi
}

test_listen_only_mode() {
    local port=$1
    log_info "  Testing listen-only mode on $port"

    set_mode $port listen-only || { log_fail "  Failed to set listen-only mode"; return 1; }

    if ! check_mode $port listen-only; then
        log_fail "  Mode flag check failed for listen-only"
        return 1
    fi

    # Verify state is ERROR-ACTIVE (not BUS-OFF)
    if ip -details link show $port 2>/dev/null | grep -q "BUS-OFF"; then
        log_fail "  Port entered BUS-OFF in listen-only mode"
        return 1
    fi

    log_success "  Listen-only mode: flag set, port active"
    return 0
}

test_mode_recovery() {
    local tx=$1 rx=$2
    log_info "  Testing mode recovery: listen-only -> normal"

    set_mode $rx listen-only || return 1
    set_mode $rx normal || return 1

    if ! check_mode $rx normal; then
        log_fail "  Failed to recover to normal mode"
        return 1
    fi

    # Verify communication restored
    set_mode $tx normal || return 1
    local logf="/tmp/mode_recovery_$$.log"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1
    sudo cansend $tx 100#AABB >/dev/null 2>&1
    wait $pid 2>/dev/null || true

    local recv; recv=$(grep -c "100" "$logf" 2>/dev/null) || recv=0
    rm -f "$logf"

    if [ "$recv" -gt 0 ]; then
        log_success "  Mode recovery: communication restored"
        return 0
    else
        log_fail "  Mode recovery: communication not restored"
        return 1
    fi
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Mode Test"
    log_info "=========================================="
    echo ""

    if [ ! -f "$RESULTS_DIR/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi
    local ports=($(cat "$RESULTS_DIR/connected_ports.txt"))
    local tx=${ports[0]} rx=${ports[1]}

    log_info "TX port: $tx"
    log_info "RX port: $rx"
    log_info "Config:  CAN FD ${MODE_BITRATE}/${MODE_DBITRATE}"
    echo ""

    backup_port_config $tx
    backup_port_config $rx

    # Test 1: Normal mode
    start_testcase "Mode_Normal"
    if test_normal_mode $tx $rx; then
        test_pass "Normal mode OK"
    else
        test_fail "Normal mode failed"
    fi
    echo ""

    # Test 2: Loopback mode
    start_testcase "Mode_Loopback"
    if test_loopback_mode $tx; then
        test_pass "Loopback mode OK"
    else
        test_fail "Loopback mode failed"
    fi
    echo ""

    # Test 3: Listen-only mode
    start_testcase "Mode_Listen_Only"
    if test_listen_only_mode $tx; then
        test_pass "Listen-only mode OK"
    else
        test_fail "Listen-only mode failed"
    fi
    echo ""

    # Test 4: Mode recovery (listen-only -> normal)
    start_testcase "Mode_Recovery"
    if test_mode_recovery $tx $rx; then
        test_pass "Mode recovery OK"
    else
        test_fail "Mode recovery failed"
    fi
    echo ""

    log_info "Restoring port configuration..."
    restore_port_config $tx
    restore_port_config $rx
    verify_port_config $tx

    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
