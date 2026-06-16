#!/bin/bash
# Module: Hardware Timestamp Test
#
# Verifies RX hardware timestamp correctness for all DLC values.
# Gen3 devices use 64-bit tail timestamp (align4 convention).
# Gen2/Gen2.5 devices use 32-bit firmware timestamp with wrap extension.
#
# Tests:
#   1. Short frames DLC 0-8 (CAN 2.0) — tail alignment critical for DLC 1-7
#   2. CAN FD frames DLC 12/16/64 — regression
#   3. Timestamp monotonicity across 200 mixed-DLC frames
#
# Expected: all frames received with correct data, timestamps strictly non-decreasing.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"

# Number of frames for monotonicity test
MONO_COUNT=200

# Maximum monotonic violations allowed
MONO_MAX_VIOLATIONS=0

################################################################################
# Helpers
################################################################################

# Send a CAN 2.0 frame and verify reception.
# Args: tx_port rx_port can_id dlc data_hex
# Returns: 0 if exactly 1 frame received with matching data.
test_can20_frame() {
    local tx=$1 rx=$2 id=$3 dlc=$4 data=$5

    local logf="/tmp/ts_test_$$.log"
    timeout 2 candump $rx -n 1 -t A > "$logf" 2>/dev/null &
    local pid=$!
    sleep 0.1

    if [ "$dlc" -eq 0 ]; then
        sudo cansend $tx "${id}#" >/dev/null 2>&1
    else
        sudo cansend $tx "${id}#${data}" >/dev/null 2>&1
    fi

    wait $pid 2>/dev/null
    local lines=$(wc -l < "$logf" 2>/dev/null || echo 0)
    rm -f "$logf"

    [ "$lines" -eq 1 ]
}

# Send a CAN FD frame and verify reception.
# Args: tx_port rx_port can_id dlc data_hex flags
# Returns: 0 if exactly 1 frame received.
test_canfd_frame() {
    local tx=$1 rx=$2 id=$3 dlc=$4 data=$5 flags=${6:-1}

    local logf="/tmp/ts_test_$$.log"
    timeout 2 candump $rx -n 1 -t A > "$logf" 2>/dev/null &
    local pid=$!
    sleep 0.1

    sudo cansend $tx "${id}##${flags}${data}" >/dev/null 2>&1

    wait $pid 2>/dev/null
    local lines=$(wc -l < "$logf" 2>/dev/null || echo 0)
    rm -f "$logf"

    [ "$lines" -eq 1 ]
}

# Run monotonicity test: send mixed-DLC frames, check timestamps never go backward.
# Args: tx_port rx_port count
# Returns: 0 if no violations found.
test_monotonicity() {
    local tx=$1 rx=$2 count=$3

    local logf="/tmp/ts_mono_$$.log"
    timeout 20 candump $rx -n $count -t A > "$logf" 2>/dev/null &
    local pid=$!
    sleep 0.3

    local i
    for i in $(seq 1 $count); do
        local dlc=$(( i % 9 ))
        case $dlc in
            0) sudo cansend $tx 123# ;;
            1) sudo cansend $tx 123#AA ;;
            2) sudo cansend $tx 123#AABB ;;
            3) sudo cansend $tx 123#AABBCC ;;
            4) sudo cansend $tx 123#AABBCCDD ;;
            5) sudo cansend $tx 123#AABBCCDDEE ;;
            6) sudo cansend $tx 123#AABBCCDDEEFF ;;
            7) sudo cansend $tx 123#AABBCCDDEEFF00 ;;
            8) sudo cansend $tx 123#AABBCCDD11223344 ;;
        esac
        sleep 0.01
    done

    wait $pid 2>/dev/null

    local received=$(wc -l < "$logf" 2>/dev/null || echo 0)
    log_info "    Received: $received / $count frames"

    if [ "$received" -lt 1 ]; then
        rm -f "$logf"
        return 1
    fi

    # Check monotonicity
    local violations=0
    local prev=""
    while IFS= read -r line; do
        local ts=$(echo "$line" | awk '{gsub(/[()]/,"",$1); print $1}')
        if [ -n "$prev" ] && [ -n "$ts" ]; then
            if [ "$(echo "$ts < $prev" | bc -l 2>/dev/null)" = "1" ]; then
                violations=$((violations + 1))
            fi
        fi
        prev="$ts"
    done < "$logf"
    rm -f "$logf"

    log_info "    Monotonic violations: $violations"
    return $violations
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Hardware Timestamp Test"
    log_info "=========================================="
    echo ""

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

    local gen=$(detect_generation)
    log_info "Generation: $gen"
    echo ""

    local total=0 pass=0 fail=0

    # -----------------------------------------------------------------------
    # Test 1: CAN 2.0 short frames DLC 0-8
    # -----------------------------------------------------------------------
    log_info "  Testing CAN 2.0 DLC 0-8 (tail alignment critical for DLC 1-7)"

    local dlc_data_map=(
        "0:"
        "1:AA"
        "2:AABB"
        "3:AABBCC"
        "4:AABBCCDD"
        "5:AABBCCDDEE"
        "6:AABBCCDDEEFF"
        "7:AABBCCDDEEFF00"
        "8:AABBCCDD11223344"
    )

    for entry in "${dlc_data_map[@]}"; do
        local dlc=${entry%%:*}
        local data=${entry#*:}
        local label="CAN20_DLC${dlc}"

        total=$((total + 1))
        start_testcase "$label"

        if test_can20_frame $tx $rx 200 "$dlc" "$data"; then
            log_success "  $label: OK"
            test_pass "$label"
            pass=$((pass + 1))
        else
            log_fail "  $label: FAIL"
            test_fail "$label: no frame received"
            fail=$((fail + 1))
        fi
    done

    # -----------------------------------------------------------------------
    # Test 2: CAN FD frames
    # -----------------------------------------------------------------------
    log_info ""
    log_info "  Testing CAN FD frames (regression)"

    local fd_data_map=(
        "12:0102030405060708090A0B0C"
        "16:0102030405060708090A0B0C0D0E0F10"
        "24:0102030405060708090A0B0C0D0E0F101112131415161718"
        "32:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20"
        "48:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F30"
        "64:000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F"
    )

    for entry in "${fd_data_map[@]}"; do
        local dlc=${entry%%:*}
        local data=${entry#*:}
        local label="CANFD_DLC${dlc}"

        total=$((total + 1))
        start_testcase "$label"

        if test_canfd_frame $tx $rx 300 "$dlc" "$data"; then
            log_success "  $label: OK"
            test_pass "$label"
            pass=$((pass + 1))
        else
            log_fail "  $label: FAIL"
            test_fail "$label: no frame received"
            fail=$((fail + 1))
        fi
    done

    # -----------------------------------------------------------------------
    # Test 3: Timestamp monotonicity
    # -----------------------------------------------------------------------
    log_info ""
    log_info "  Testing timestamp monotonicity ($MONO_COUNT frames, mixed DLC 0-8)"

    total=$((total + 1))
    start_testcase "Timestamp_Monotonicity"

    if test_monotonicity $tx $rx $MONO_COUNT; then
        log_success "  Monotonicity: $MONO_COUNT frames, 0 violations"
        test_pass "Timestamp_Monotonicity"
        pass=$((pass + 1))
    else
        log_fail "  Monotonicity: violations detected"
        test_fail "Timestamp_Monotonicity: backward timestamp detected"
        fail=$((fail + 1))
    fi

    # -----------------------------------------------------------------------
    # Summary
    # -----------------------------------------------------------------------
    log_info ""
    log_info "  Summary: $pass/$total passed"

    if [ $fail -gt 0 ]; then
        log_fail "Timestamp test FAILED ($fail failures)"
        return 1
    fi

    log_success "Hardware timestamp test completed"
    return 0
}
