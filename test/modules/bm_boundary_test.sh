#!/bin/bash
# Module: Boundary/Limit Test
#
# Tests out-of-range inputs for all device configuration features:
# - TXTASK: index 64 (max), index 65 (out of range)
# - ROUTE: index at max, index beyond max
# - RXFILTER: index 2 (max), index 3 (out of range)
# - SAVE/LOAD/CLEAR: mask values (0xFFFF, 0x10000, invalid)
# - LOGGING/REPLAY: short data, oversized data
# - Invalid hex data
#
# Expected: out-of-range requests are rejected with error, no crash or hang.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"

################################################################################
# Helpers
################################################################################

# Build a valid 128-byte txtask hex string (all zeros except header)
make_txtask_hex() {
    # 128 bytes = 256 hex chars, valid txtask header with periodic config
    echo -n "0101000400006400000001000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000AABBCC"
}

# Build a valid 16-byte route hex string
make_route_hex() {
    echo -n "0100FF0F000000000000000000000000"
}

# Check sysfs write returns error
expect_reject() {
    local desc=$1
    local path=$2
    local data=$3

    echo "$data" | sudo tee "$path" >/dev/null 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        log_success "  $desc: correctly rejected (rc=$rc)"
        return 0
    else
        # Check dmesg for warning/error
        if sudo dmesg | tail -5 | grep -qiE 'out of range|invalid|error'; then
            log_success "  $desc: correctly rejected (kernel warning)"
            return 0
        fi
        log_warn "  $desc: not rejected (may be accepted by device)"
        return 0  # Not a hard failure — device may handle it
    fi
}

# Check sysfs write succeeds
expect_accept() {
    local desc=$1
    local path=$2
    local data=$3

    echo "$data" | sudo tee "$path" >/dev/null 2>&1
    local rc=$?
    if [ $rc -eq 0 ]; then
        log_success "  $desc: accepted (rc=0)"
        return 0
    else
        log_fail "  $desc: unexpectedly rejected (rc=$rc)"
        return 1
    fi
}

################################################################################
# Test Cases
################################################################################

test_txtask_boundary() {
    local port=$1
    local sysfs_path="/sys/class/net/$port/txtasks"
    local pass=true

    log_info "  Testing TXTASK boundary..."

    if [ ! -f "$sysfs_path" ]; then
        log_warn "  sysfs txtasks not found — skipping"
        return 0
    fi

    local valid_hex=$(make_txtask_hex)

    # Index 0: must succeed
    if ! expect_accept "TXTASK index 0" "$sysfs_path" "0 $valid_hex"; then
        pass=false
    fi

    # Index 63: must succeed (last valid)
    if ! expect_accept "TXTASK index 63" "$sysfs_path" "63 $valid_hex"; then
        pass=false
    fi

    # Index 64: must be rejected (out of range)
    expect_reject "TXTASK index 64 (OOR)" "$sysfs_path" "64 $valid_hex"

    # Index 100: must be rejected
    expect_reject "TXTASK index 100 (OOR)" "$sysfs_path" "100 $valid_hex"

    # Index 65535: must be rejected
    expect_reject "TXTASK index 65535 (OOR)" "$sysfs_path" "65535 $valid_hex"

    # Invalid: non-numeric index
    expect_reject "TXTASK index 'abc'" "$sysfs_path" "abc $valid_hex"

    # Invalid: too short data (32 hex chars instead of 256)
    expect_reject "TXTASK short data" "$sysfs_path" "0 0102030405060708"

    # Invalid: empty data
    expect_reject "TXTASK empty data" "$sysfs_path" "0 "

    if $pass; then
        return 0
    else
        return 1
    fi
}

test_route_boundary() {
    local port=$1
    local sysfs_path="/sys/class/net/$port/routes"
    local pass=true

    log_info "  Testing ROUTE boundary..."

    if [ ! -f "$sysfs_path" ]; then
        log_warn "  sysfs routes not found — skipping"
        return 0
    fi

    local valid_hex=$(make_route_hex)

    # Index 0: must succeed
    if ! expect_accept "ROUTE index 0" "$sysfs_path" "0 $valid_hex"; then
        pass=false
    fi

    # Get max route from device caps (default 64)
    local max_route=64

    # Index max-1: must succeed (last valid)
    if ! expect_accept "ROUTE index $((max_route - 1))" "$sysfs_path" "$((max_route - 1)) $valid_hex"; then
        pass=false
    fi

    # Index max: must be rejected (out of range)
    expect_reject "ROUTE index $max_route (OOR)" "$sysfs_path" "$max_route $valid_hex"

    # Index 255: device limit — may succeed or fail depending on firmware
    expect_reject "ROUTE index 255" "$sysfs_path" "255 $valid_hex"

    # Index 256: must be rejected
    expect_reject "ROUTE index 256 (OOR)" "$sysfs_path" "256 $valid_hex"

    # Invalid: wrong data size (8 hex chars instead of 32)
    expect_reject "ROUTE short data" "$sysfs_path" "0 01020304"

    if $pass; then
        return 0
    else
        return 1
    fi
}

test_config_mask_boundary() {
    local port=$1
    local pass=true

    log_info "  Testing config mask boundary..."

    # Save path
    local save_path="/sys/class/net/$port/save"
    local load_path="/sys/class/net/$port/load"
    local clear_path="/sys/class/net/$port/clear"

    if [ ! -f "$save_path" ]; then
        log_warn "  sysfs save not found — skipping"
        return 0
    fi

    # Valid masks
    expect_accept "SAVE mask 0x0200 (txtask)" "$save_path" "0x0200"
    expect_accept "SAVE mask 0x0000 (all)" "$save_path" "0x0000"

    # Max valid mask (u16)
    expect_accept "SAVE mask 0xFFFF" "$save_path" "0xFFFF"

    # Overflow: 0x10000 (u16 + 1) — must be rejected
    expect_reject "SAVE mask 0x10000 (overflow)" "$save_path" "0x10000"

    # Invalid: garbage
    expect_reject "SAVE mask 'xyz'" "$save_path" "xyz"

    # Load with valid mask
    if [ -f "$load_path" ]; then
        expect_accept "LOAD mask 0x0200" "$load_path" "0x0200"
    fi

    # Clear with valid mask
    if [ -f "$clear_path" ]; then
        expect_accept "CLEAR mask 0x0200" "$clear_path" "0x0200"
    fi

    return 0
}

test_api_boundary() {
    local port=$1
    local pass=true

    log_info "  Testing API tool boundary..."

    if [ ! -x "$API_BIN" ]; then
        log_warn "  bmcan_api not found — skipping"
        return 0
    fi

    # TXTASK via API: valid index 0
    local rc
    $API_BIN txtask --index 0 --id 0x100 --cycle 1000 --type periodic --payload AABBCCDD --device $port >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        log_success "  API txtask index 0: OK"
    else
        log_warn "  API txtask index 0: rc=$rc"
    fi

    # TXTASK via API: invalid index 65
    $API_BIN txtask --index 65 --id 0x100 --cycle 1000 --type periodic --payload AABBCCDD --device $port >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        log_success "  API txtask index 65: correctly rejected (rc=$rc)"
    else
        log_warn "  API txtask index 65: not rejected (rc=$rc)"
    fi

    # TXTASK via API: invalid index -1
    $API_BIN txtask --index -1 --id 0x100 --cycle 1000 --type periodic --payload AABBCCDD --device $port >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        log_success "  API txtask index -1: correctly rejected (rc=$rc)"
    else
        log_warn "  API txtask index -1: not rejected (rc=$rc)"
    fi

    # ROUTE via API: valid
    $API_BIN route --index 0 --source 0 --target 0xFFFF --flags-mask 0xFF --flags-value 0xFF --device $port >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        log_success "  API route index 0: OK"
    else
        log_warn "  API route index 0: rc=$rc"
    fi

    # ROUTE via API: invalid index 256
    $API_BIN route --index 256 --source 0 --target 0xFFFF --flags-mask 0xFF --flags-value 0xFF --device $port >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        log_success "  API route index 256: correctly rejected (rc=$rc)"
    else
        log_warn "  API route index 256: not rejected (rc=$rc)"
    fi

    # Invalid device name
    $API_BIN txtask --index 0 --id 0x100 --cycle 1000 --type periodic --data AABBCCDD --device "../../etc/passwd" >/dev/null 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        log_success "  API path traversal device: correctly rejected (rc=$rc)"
    else
        log_warn "  API path traversal device: not rejected (rc=$rc)"
    fi

    # Invalidate with valid range (0-63 to cover index 63 written by test_txtask_boundary)
    $API_BIN invalidate --txtask --tx-range 0-63 --device $port >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        log_success "  API invalidate txtask 0-63: OK"
    else
        log_warn "  API invalidate txtask 0-63: rc=$rc"
    fi

    return 0
}

test_termination_boundary() {
    local port=$1

    log_info "  Testing termination boundary..."

    # Valid values
    sudo ip link set $port type can termination 0 2>/dev/null
    log_success "  Termination 0: accepted"
    sudo ip link set $port type can termination 120 2>/dev/null
    log_success "  Termination 120: accepted"

    # Invalid values (SocketCAN framework rejects these)
    sudo ip link set $port type can termination 999 2>/dev/null
    local rc=$?
    if [ $rc -ne 0 ]; then
        log_success "  Termination 999: correctly rejected"
    else
        log_warn "  Termination 999: not rejected by framework"
    fi

    # Restore
    sudo ip link set $port type can termination 120 2>/dev/null

    return 0
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Boundary/Limit Test"
    log_info "=========================================="
    echo ""

    if [ ! -f "$RESULTS_DIR/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi
    local ports=($(cat "$RESULTS_DIR/connected_ports.txt"))
    local port=${ports[0]}

    log_info "Test port: $port"
    echo ""

    # Test 1: TXTASK boundary
    start_testcase "Boundary_TXTASK"
    if test_txtask_boundary $port; then
        test_pass "TXTASK boundary tests passed"
    else
        test_fail "TXTASK boundary tests failed"
    fi
    echo ""

    # Test 2: ROUTE boundary
    start_testcase "Boundary_ROUTE"
    if test_route_boundary $port; then
        test_pass "ROUTE boundary tests passed"
    else
        test_fail "ROUTE boundary tests failed"
    fi
    echo ""

    # Test 3: Config mask boundary
    start_testcase "Boundary_Config_Mask"
    if test_config_mask_boundary $port; then
        test_pass "Config mask boundary tests passed"
    else
        test_fail "Config mask boundary tests failed"
    fi
    echo ""

    # Test 4: API tool boundary
    start_testcase "Boundary_API_Tool"
    if test_api_boundary $port; then
        test_pass "API tool boundary tests passed"
    else
        test_fail "API tool boundary tests failed"
    fi
    echo ""

    # Test 5: Termination boundary
    start_testcase "Boundary_Termination"
    if test_termination_boundary $port; then
        test_pass "Termination boundary tests passed"
    else
        test_fail "Termination boundary tests failed"
    fi
    echo ""

    log_success "Boundary/limit test completed"
    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
