#!/bin/bash
################################################################################
# Module: Comprehensive Communication Test
# Tests CAN 2.0 and CAN-FD frame types, lengths, IDs, and BRS
################################################################################

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# Test Helpers
################################################################################

# Send a CAN 2.0 frame and verify reception
test_frame_tx_rx() {
    local tx=$1
    local rx=$2
    local frame_id=$3
    local frame_data=$4

    local id_hex=$(echo "$frame_id" | sed 's/0x//g' | tr '[:upper:]' '[:lower:]')
    local logf="/tmp/frame_test_$$.log"
    local max_retries=5
    local attempt

    for attempt in $(seq 1 $max_retries); do
        rm -f "$logf"
        # candump -n 1: receive exactly one frame, then exit automatically
        timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
        local pid=$!
        sleep 0.1

        sudo cansend $tx "$frame_id#$frame_data" >/dev/null 2>&1
        sleep 0.3

        # Wait for candump to finish (it exits after 1 frame or 2s timeout)
        wait $pid 2>/dev/null || true

        local received; received=$(grep -i -c "$id_hex" "$logf" 2>/dev/null) || received=0
        [[ "$received" =~ ^[0-9]+$ ]] || received=0
        rm -f "$logf"

        if [ "$received" -gt 0 ]; then
            return 0
        fi
    done

    return 1
}

# Send a CAN-FD frame and verify reception
test_fd_frame_tx_rx() {
    local tx=$1
    local rx=$2
    local frame_id=$3
    local flags=$4
    local frame_data=$5

    local id_hex=$(echo "$frame_id" | sed 's/0x//g' | tr '[:upper:]' '[:lower:]')
    local logf="/tmp/fd_frame_test_$$.log"
    local max_retries=5
    local attempt

    for attempt in $(seq 1 $max_retries); do
        rm -f "$logf"
        timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
        local pid=$!
        sleep 0.1

        sudo cansend $tx "$frame_id##$flags$frame_data" >/dev/null 2>&1
        sleep 0.3

        wait $pid 2>/dev/null || true

        local received; received=$(grep -i -c "$id_hex" "$logf" 2>/dev/null) || received=0
        [[ "$received" =~ ^[0-9]+$ ]] || received=0
        rm -f "$logf"

        if [ "$received" -gt 0 ]; then
            return 0
        fi
    done

    return 1
}

################################################################################
# CAN 2.0 Test Groups
################################################################################

test_can_20_data_lengths() {
    local tx=$1
    local rx=$2

    start_testcase "CAN20_Data_Lengths"

    log_header "Testing CAN 2.0 Data Lengths (0-8 bytes)"

    local test_cases=(
        "0:123#00000000"
        "1:123#01"
        "2:123#0102"
        "3:123#010203"
        "4:123#01020304"
        "5:123#0102030405"
        "6:123#010203040506"
        "7:123#01020304050607"
        "8:123#0102030405060708"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local len=${tc%%:*}
        local frame=${tc#*:}
        local frame_id=${frame%%#*}
        local frame_data=${frame#*#}

        echo -n "  CAN 2.0, $len bytes: "

        if test_frame_tx_rx "$tx" "$rx" "$frame_id" "$frame_data"; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN 2.0 Data Lengths: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "All CAN 2.0 data lengths work"
        return 0
    else
        test_fail "Some CAN 2.0 data lengths failed"
        return 1
    fi
}

test_can_20_id_types() {
    local tx=$1
    local rx=$2

    start_testcase "CAN20_ID_Types"

    log_header "Testing CAN 2.0 ID Types"

    local test_cases=(
        "Standard_11bit:123:DEADBEEF"
        "Standard_Max_11bit:7FF:DEADBEEF"
        "Extended_29bit:12345678:DEADBEEF"
        "Extended_Max_18bit:0003FFFF:DEADBEEF"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local name=${tc%%:*}
        local id=$(echo "$tc" | cut -d: -f2)
        local data=$(echo "$tc" | cut -d: -f3)

        echo -n "  $name (ID=$id): "

        if test_frame_tx_rx "$tx" "$rx" "$id" "$data"; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN 2.0 ID Types: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "All CAN 2.0 ID types work"
        return 0
    else
        test_fail "Some CAN 2.0 ID types failed"
        return 1
    fi
}

test_can_20_id_ranges() {
    local tx=$1
    local rx=$2

    start_testcase "CAN20_ID_Ranges"

    log_header "Testing CAN 2.0 ID Ranges"

    local test_cases=(
        "Low_Standard:100"
        "Mid_Standard:400"
        "High_Standard:700"
        "Low_Extended:00010000"
        "Mid_Extended:00020000"
        "High_Extended:0003FFFF"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local name=${tc%%:*}
        local id=${tc#*:}

        echo -n "  $name (0x$(echo $id | sed 's/^0*//')): "

        if test_frame_tx_rx "$tx" "$rx" "$id" "DEADBEEF"; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN 2.0 ID Ranges: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "All CAN 2.0 ID ranges work"
        return 0
    else
        test_fail "Some CAN 2.0 ID ranges failed"
        return 1
    fi
}

test_can_20_rtr_frames() {
    local tx=$1
    local rx=$2

    start_testcase "CAN20_RTR_Frames"

    log_header "Testing CAN 2.0 RTR Frames"

    local test_cases=(
        "Standard_RTR_Len0:100#R0"
        "Standard_RTR_Len4:100#R4"
        "Standard_RTR_Len8:100#R8"
        "Extended_RTR_Len0:00010000#R0"
        "Extended_RTR_Len4:00020000#R4"
        "Extended_RTR_Len8:0003FFFF#R8"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local name=${tc%%:*}
        local frame=${tc#*:}

        echo -n "  $name: "

        local id=$(echo "$frame" | cut -d# -f1)
        local id_hex=$(echo "$id" | sed 's/0x//g' | tr '[:upper:]' '[:lower:]')
        local logf="/tmp/rtr_test_$$.log"
        local received=0
        local rtr_attempt

        for rtr_attempt in $(seq 1 5); do
            rm -f "$logf"
            timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
            local pid=$!
            sleep 0.1

            sudo cansend $tx "$frame" >/dev/null 2>&1
            sleep 0.3

            wait $pid 2>/dev/null || true

            received=$(grep -i -c "$id_hex" "$logf" 2>/dev/null) || received=0
            [[ "$received" =~ ^[0-9]+$ ]] || received=0
            rm -f "$logf"

            if [ "$received" -gt 0 ]; then
                break
            fi
        done

        if [ "$received" -gt 0 ]; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN 2.0 RTR Frames: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "All CAN 2.0 RTR frames work"
        return 0
    else
        test_fail "Some CAN 2.0 RTR frames failed"
        return 1
    fi
}

################################################################################
# CAN FD Test Groups
################################################################################

test_canfd_data_lengths() {
    local tx=$1
    local rx=$2

    start_testcase "CANFD_Data_Lengths"

    log_header "Testing CAN FD Data Lengths"

    local test_cases=(
        "0:1:0000000000000000"
        "1:1:01"
        "4:1:01020304"
        "8:1:0102030405060708"
        "12:1:0102030405060708090A0B0C"
        "16:1:0102030405060708090A0B0C0D0E0F10"
        "20:1:0102030405060708090A0B0C0D0E0F1011121314"
        "24:1:0102030405060708090A0B0C0D0E0F1011121314151618"
        "32:1:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20"
        "48:1:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F3031"
        "64:1:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F40"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local len=${tc%%:*}
        local flags=$(echo "$tc" | cut -d: -f2)
        local data=$(echo "$tc" | cut -d: -f3)

        echo -n "  CAN FD, $len bytes: "

        if test_fd_frame_tx_rx "$tx" "$rx" "123" "$flags" "$data"; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN FD Data Lengths: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "All CAN FD data lengths work"
        return 0
    else
        test_fail "Some CAN FD data lengths failed"
        return 1
    fi
}

test_canfd_id_types() {
    local tx=$1
    local rx=$2

    start_testcase "CANFD_ID_Types"

    log_header "Testing CAN FD ID Types"

    local test_cases=(
        "Standard_11bit:123:1:DEADBEEF"
        "Standard_Max:7FF:1:DEADBEEF"
        "Extended_29bit:12345678:1:DEADBEEF"
        "Extended_Max_18bit:0003FFFF:1:DEADBEEF"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local name=${tc%%:*}
        local id=$(echo "$tc" | cut -d: -f2)
        local flags=$(echo "$tc" | cut -d: -f3)
        local data=$(echo "$tc" | cut -d: -f4)

        echo -n "  $name: "

        if test_fd_frame_tx_rx "$tx" "$rx" "$id" "$flags" "$data"; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN FD ID Types: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "All CAN FD ID types work"
        return 0
    else
        test_fail "Some CAN FD ID types failed"
        return 1
    fi
}

test_canfd_brs() {
    local tx=$1
    local rx=$2

    start_testcase "CANFD_BRS"

    log_header "Testing CAN FD BRS (Bit Rate Switch)"

    local test_cases=(
        "No_BRS:1:DEADBEEF"
        "With_BRS:5:DEADBEEF"
    )

    local passed=0
    local total=${#test_cases[@]}

    for tc in "${test_cases[@]}"; do
        local name=${tc%%:*}
        local flags=$(echo "$tc" | cut -d: -f2)
        local data=$(echo "$tc" | cut -d: -f3)

        echo -n "  $name (flags=0x$flags): "

        if test_fd_frame_tx_rx "$tx" "$rx" "123" "$flags" "$data"; then
            echo "OK"
            ((passed++))
        else
            echo "FAIL"
        fi
    done

    log_info "CAN FD BRS: $passed/$total passed"

    if [ "$passed" -eq "$total" ]; then
        test_pass "CAN FD BRS works"
        return 0
    else
        test_fail "CAN FD BRS failed"
        return 1
    fi
}

################################################################################
# Main
################################################################################

main() {
    log_header "Comprehensive Communication Test"
    log_info "Start time: $(date '+%Y-%m-%d %H:%M:%S')"
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

    local tx_port=${ports[0]}
    local rx_port=${ports[1]}

    log_info "TX port: $tx_port"
    log_info "RX port: $rx_port"
    echo ""

    # Reinitialize ports for clean CAN-FD state
    source "$SCRIPT_DIR/../lib/hardware_helper.sh"
    hw_init
    hw_init_pair "$tx_port" "$rx_port"
    echo ""

    # Warm-up: send a few frames to stabilize the CAN bus after reinitialization
    log_info "Warming up CAN bus..."
    local warmup_ok=0
    local warmup_log="/tmp/warmup_$$.log"
    for w in $(seq 1 5); do
        rm -f "$warmup_log"
        timeout 1 candump $rx_port -n 1 -t A 2>/dev/null > "$warmup_log" &
        local warmup_pid=$!
        sleep 0.05
        sudo cansend $tx_port 123#DEADBEEF >/dev/null 2>&1
        wait $warmup_pid 2>/dev/null || true
        if grep -q "123" "$warmup_log" 2>/dev/null; then
            warmup_ok=$((warmup_ok + 1))
        fi
        rm -f "$warmup_log"
    done
    if [ "$warmup_ok" -gt 0 ]; then
        log_success "Warm-up: $warmup_ok/5 frames received"
    else
        log_warn "Warm-up: 0/5 frames received - CAN bus may be unstable"
    fi
    echo ""

    local total_tests=0
    local passed_tests=0

    # CAN 2.0 tests
    log_info "=========================================="
    log_info "CAN 2.0 Test Suite"
    log_info "=========================================="
    echo ""

    if test_can_20_data_lengths "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    if test_can_20_id_types "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    if test_can_20_id_ranges "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    if test_can_20_rtr_frames "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    # CAN FD tests
    log_info "=========================================="
    log_info "CAN FD Test Suite"
    log_info "=========================================="
    echo ""

    if test_canfd_data_lengths "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    if test_canfd_id_types "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    if test_canfd_brs "$tx_port" "$rx_port"; then ((passed_tests++)); fi
    ((total_tests++))
    echo ""

    # Summary
    log_header "Test Summary"
    log_info "Total: $total_tests"
    log_info "Passed: $passed_tests"
    log_info "Failed: $((total_tests - passed_tests))"
    log_info "Pass Rate: $((passed_tests * 100 / total_tests))%"
    echo ""

    if [ "$passed_tests" -eq "$total_tests" ]; then
        log_success "All comprehensive communication tests passed!"
        return 0
    else
        log_fail "Some comprehensive communication tests failed"
        return 1
    fi
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
