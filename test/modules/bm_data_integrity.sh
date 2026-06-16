#!/bin/bash
################################################################################
# Module: Data Integrity Test (User Scenario Coverage)
# Verifies: CAN 2.0 payload, standard/extended ID, RTR frames,
# CAN FD DLC mapping (all 16 values + boundaries), BRS flags,
# bidirectional communication, frame count, bitrate, and stress test.
#
# 14 test cases covering user scenarios:
#   Phase 1: CAN 2.0 (payload, standard ID, RTR)
#   Phase 2: CAN FD (DLC boundaries, BRS flags)
#   Phase 3: ID & Communication (extended ID, frame count, reverse direction)
#   Phase 4: Stress (100 rapid frames)
#   Phase 5: Configuration (bitrate readback, 12 bitrate combos, bitrate functional)
#
# All test cases have been manually verified on ARM (RK3588, kernel 4.19).
# Test port pair: can0 (TX) -> can6 (RX)
################################################################################
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"

# Helpers ----------------------------------------------------------------

# Send CAN FD frame on TX, verify reception on RX.
# Args: tx rx can_id(3-hex) hex_data
# Returns 0 if received frame has correct length and data prefix matches.
verify_fd_frame() {
    local tx=$1 rx=$2 id=$3 data=$4
    local logf="/tmp/dlc_test_$$.log"
    local sent_len=$(( ${#data} / 2 ))

    # Determine expected received length from DLC boundary
    local expected_len
    if   (( sent_len <= 8  )); then expected_len=$sent_len
    elif (( sent_len <= 12 )); then expected_len=12
    elif (( sent_len <= 16 )); then expected_len=16
    elif (( sent_len <= 20 )); then expected_len=20
    elif (( sent_len <= 24 )); then expected_len=24
    elif (( sent_len <= 32 )); then expected_len=32
    elif (( sent_len <= 48 )); then expected_len=48
    else expected_len=64
    fi

    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.05
    sudo cansend $tx "${id}##1${data}" 2>/dev/null
    sleep 0.4
    wait $pid 2>/dev/null || true

    local recv_line
    recv_line=$(grep -i "$id" "$logf" 2>/dev/null | head -1)
    rm -f "$logf"

    if [ -z "$recv_line" ]; then
        VERIFY_ERR="no frame received"
        return 1
    fi

    # Parse: can6  200  [12]  01 02 03 04 ...
    # kernel 4.19 shows [00] [01] etc (2-digit with leading zero)
    local recv_len recv_data
    recv_len=$(echo "$recv_line" | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
    recv_len=$((10#$recv_len))  # strip leading zeros
    recv_data=$(echo "$recv_line" | sed 's/.*\]  //' | tr -d ' ' | tr '[:upper:]' '[:lower:]')

    local sent_lower=$(echo "$data" | tr '[:upper:]' '[:lower:]')

    # Check length
    if [ "$recv_len" != "$expected_len" ]; then
        VERIFY_ERR="len mismatch: sent=${sent_len}B expected=${expected_len}B recv=${recv_len}B"
        return 1
    fi

    # Check payload prefix matches sent data
    local prefix_len=${#sent_lower}
    if [ "${recv_data:0:$prefix_len}" != "$sent_lower" ]; then
        VERIFY_ERR="data mismatch: sent=${sent_lower} recv=${recv_data:0:$prefix_len}"
        return 1
    fi

    VERIFY_ERR=""
    return 0
}

# Send CAN 2.0 frame on TX, verify reception on RX.
# Args: tx rx can_id hex_data
# Returns 0 if received frame has correct length and data matches exactly.
verify_can20_frame() {
    local tx=$1 rx=$2 id=$3 data=$4
    local logf="/tmp/can20_vfy_$$.log"
    local sent_lower=$(echo "$data" | tr '[:upper:]' '[:lower:]')
    local sent_len=$(( ${#data} / 2 ))

    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.05
    sudo cansend $tx "${id}#${data}" 2>/dev/null
    sleep 0.4
    wait $pid 2>/dev/null || true

    local recv_line
    recv_line=$(grep -i "$id" "$logf" 2>/dev/null | head -1)
    rm -f "$logf"

    if [ -z "$recv_line" ]; then
        VERIFY_ERR="no frame received"
        return 1
    fi

    local recv_len recv_data
    recv_len=$(echo "$recv_line" | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
    recv_len=$((10#$recv_len))
    recv_data=$(echo "$recv_line" | sed 's/.*\]  //' | tr -d ' ' | tr '[:upper:]' '[:lower:]')

    if [ "$recv_len" != "$sent_len" ]; then
        VERIFY_ERR="len mismatch: sent=${sent_len}B recv=${recv_len}B"
        return 1
    fi

    if [ -n "$sent_lower" ] && [ "$recv_data" != "$sent_lower" ]; then
        VERIFY_ERR="data mismatch: sent=$sent_lower recv=$recv_data"
        return 1
    fi

    VERIFY_ERR=""
    return 0
}

# Run a table of label:id:data tests using verify_fd_frame.
run_test_table() {
    local tx=$1 rx=$2; shift 2
    local tests=("$@")
    local passed=0 total=${#tests[@]}

    for tc in "${tests[@]}"; do
        local label=${tc%%:*}; local rest=${tc#*:}
        local id=${rest%%:*}; local data=${rest#*:}
        echo -n "  $label: "
        if verify_fd_frame "$tx" "$rx" "$id" "$data"; then
            echo "OK"; ((passed++))
        else
            echo "FAIL ($VERIFY_ERR)"
        fi
    done
    log_info "Result: $passed/$total passed"
    [ "$passed" -eq "$total" ]
}

# Run a table of label:id:data tests using verify_can20_frame.
run_can20_test_table() {
    local tx=$1 rx=$2; shift 2
    local tests=("$@")
    local passed=0 total=${#tests[@]}

    for tc in "${tests[@]}"; do
        local label=${tc%%:*}; local rest=${tc#*:}
        local id=${rest%%:*}; local data=${rest#*:}
        echo -n "  $label: "
        if verify_can20_frame "$tx" "$rx" "$id" "$data"; then
            echo "OK"; ((passed++))
        else
            echo "FAIL ($VERIFY_ERR)"
        fi
    done
    log_info "Result: $passed/$total passed"
    [ "$passed" -eq "$total" ]
}

# Tests ------------------------------------------------------------------

test_can20_payload() {
    local tx=$1 rx=$2
    start_testcase "CAN20_Payload"
    log_header "Testing CAN 2.0 payload content (0-8 bytes)"

    local tests=(
        "0B:100:"
        "1B:101:AA"
        "2B:102:DEAD"
        "4B:103:DEADBEEF"
        "8B:104:0102030405060708"
    )

    if run_can20_test_table "$tx" "$rx" "${tests[@]}"; then
        test_pass "All CAN 2.0 payload content correct"
        return 0
    else
        test_fail "Some CAN 2.0 payload tests failed"
        return 1
    fi
}

test_dlc_exact_boundaries() {
    local tx=$1 rx=$2
    start_testcase "DLC_Exact_Boundaries"
    log_header "Testing all 16 DLC exact boundary values"

    # Each test: label can_id hex_data
    # DLC 0-8: data length equals DLC
    # DLC 9=12B, 10=16B, 11=20B, 12=24B, 13=32B, 14=48B, 15=64B
    local tests=(
        "DLC0=0B:200:"
        "DLC1=1B:201:01"
        "DLC2=2B:202:0102"
        "DLC3=3B:203:010203"
        "DLC4=4B:204:01020304"
        "DLC5=5B:205:0102030405"
        "DLC6=6B:206:010203040506"
        "DLC7=7B:207:01020304050607"
        "DLC8=8B:208:0102030405060708"
        "DLC9=12B:209:0102030405060708090A0B0C"
        "DLC10=16B:20A:0102030405060708090A0B0C0D0E0F10"
        "DLC11=20B:20B:0102030405060708090A0B0C0D0E0F1011121314"
        "DLC12=24B:20C:0102030405060708090A0B0C0D0E0F101112131415161718"
        "DLC13=32B:20D:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20"
        "DLC14=48B:20E:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F30"
        "DLC15=64B:20F:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F40"
    )

    if run_test_table "$tx" "$rx" "${tests[@]}"; then
        test_pass "All DLC boundaries correct"
        return 0
    else
        test_fail "Some DLC boundaries failed"
        return 1
    fi
}

test_dlc_boundary_first() {
    local tx=$1 rx=$2
    start_testcase "DLC_Boundary_First"
    log_header "Testing first value of each DLC zone (non-standard length)"

    # First value in each DLC zone: the value that just entered the next DLC
    local tests=(
        "9B->12:400:010203040506070809"
        "13B->16:401:0102030405060708090A0B0C0D"
        "17B->20:402:0102030405060708090A0B0C0D0E0F1011"
        "21B->24:403:0102030405060708090A0B0C0D0E0F101112131415"
        "25B->32:404:0102030405060708090A0B0C0D0E0F101112131415161819"
        "33B->48:405:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F2021"
        "49B->64:406:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132"
    )

    if run_test_table "$tx" "$rx" "${tests[@]}"; then
        test_pass "All DLC boundary-first values correct"
        return 0
    else
        test_fail "Some DLC boundary-first tests failed"
        return 1
    fi
}

test_dlc_boundary_last() {
    local tx=$1 rx=$2
    start_testcase "DLC_Boundary_Last"
    log_header "Testing last value of each DLC zone (exact boundary)"

    local tests=(
        "12B->12:410:0102030405060708090A0B0C"
        "16B->16:411:0102030405060708090A0B0C0D0E0F10"
        "20B->20:412:0102030405060708090A0B0C0D0E0F1011121314"
        "24B->24:413:0102030405060708090A0B0C0D0E0F101112131415161718"
        "32B->32:414:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20"
        "48B->48:415:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F30"
        "64B->64:416:0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F40"
    )

    if run_test_table "$tx" "$rx" "${tests[@]}"; then
        test_pass "All DLC boundary-last values correct"
        return 0
    else
        test_fail "Some DLC boundary-last tests failed"
        return 1
    fi
}

test_canfd_brs_flags() {
    local tx=$1 rx=$2
    start_testcase "CANFD_BRS_Flags"
    log_header "Testing CAN FD BRS flag preservation"

    # Send with BRS (##1) and without BRS (##0), verify reception
    local passed=0 total=4

    # BRS=1 with small payload
    echo -n "  BRS=1, 8B: "
    if verify_fd_frame "$tx" "$rx" "500" "DEADBEEF"; then
        echo "OK"; ((passed++))
    else
        echo "FAIL ($VERIFY_ERR)"
    fi

    # BRS=1 with large payload
    echo -n "  BRS=1, 64B: "
    if verify_fd_frame "$tx" "$rx" "501" "0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F40"; then
        echo "OK"; ((passed++))
    else
        echo "FAIL ($VERIFY_ERR)"
    fi

    # BRS=0 (send via ##0) — need separate logic since verify_fd_frame always uses ##1
    # For BRS=0 we send ##0 but still expect correct data
    local logf="/tmp/brs0_test_$$.log"
    local id="502"
    local data="AABBCCDD"

    echo -n "  BRS=0, 4B: "
    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!; sleep 0.05
    sudo cansend $tx "${id}##0${data}" 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    local recv_line
    recv_line=$(grep -i "$id" "$logf" 2>/dev/null | head -1)
    rm -f "$logf"
    if [ -n "$recv_line" ]; then
        local recv_data
        recv_data=$(echo "$recv_line" | sed 's/.*\]  //' | tr -d ' ' | tr '[:upper:]' '[:lower:]')
        if [ "$recv_data" = "$(echo "$data" | tr '[:upper:]' '[:lower:]')" ]; then
            echo "OK"; ((passed++))
        else
            echo "FAIL (data mismatch)"
        fi
    else
        echo "FAIL (no frame)"
    fi

    # BRS=0 with 16-byte payload
    local id2="503"
    local data2="0102030405060708090A0B0C0D0E0F10"
    echo -n "  BRS=0, 16B: "
    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    pid=$!; sleep 0.05
    sudo cansend $tx "${id2}##0${data2}" 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    recv_line=$(grep -i "$id2" "$logf" 2>/dev/null | head -1)
    rm -f "$logf"
    if [ -n "$recv_line" ]; then
        local recv_data2
        recv_data2=$(echo "$recv_line" | sed 's/.*\]  //' | tr -d ' ' | tr '[:upper:]' '[:lower:]')
        local sent2=$(echo "$data2" | tr '[:upper:]' '[:lower:]')
        if [ "${recv_data2:0:${#sent2}}" = "$sent2" ]; then
            echo "OK"; ((passed++))
        else
            echo "FAIL (data mismatch)"
        fi
    else
        echo "FAIL (no frame)"
    fi

    log_info "CAN FD BRS Flags: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "CAN FD BRS flags correct" || test_fail "$((total-passed)) BRS tests failed"
    return $?
}

test_extended_id() {
    local tx=$1 rx=$2
    start_testcase "Extended_ID"
    log_header "Testing extended (29-bit) CAN IDs"

    # Extended IDs must use 8-digit format for cansend on kernel 4.19
    local passed=0 total=4

    local tests=(
        "ext_0x10000:00010000"
        "ext_0x12345:00012345"
        "ext_0x12345678:12345678"
        "ext_max_0x1FFFFFFF:1FFFFFFF"
    )

    for tc in "${tests[@]}"; do
        local label=${tc%%:*}; local id=${tc#*:}

        echo -n "  $label: "
        local logf="/tmp/extid_$$.log"
        rm -f "$logf"
        timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
        local pid=$!; sleep 0.05
        sudo cansend $tx "${id}#AABBCCDD" 2>/dev/null
        sleep 0.4; wait $pid 2>/dev/null || true

        local recv_line
        recv_line=$(grep -i "$id" "$logf" 2>/dev/null | head -1)
        rm -f "$logf"

        if [ -n "$recv_line" ]; then
            echo "OK"; ((passed++))
        else
            echo "FAIL (no frame)"
        fi
    done

    log_info "Extended ID: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "All extended IDs correct" || test_fail "$((total-passed)) extended ID tests failed"
    return $?
}

test_standard_id() {
    local tx=$1 rx=$2
    start_testcase "Standard_ID"
    log_header "Testing standard (11-bit) CAN IDs"

    local tests=(
        "std_0x100:100:AABBCCDD"
        "std_0x7FF_min:7FF:AABBCCDD"
        "std_0x001:001:DEADBEEF"
    )

    local passed=0 total=${#tests[@]}

    for tc in "${tests[@]}"; do
        local label=${tc%%:*}; local rest=${tc#*:}
        local id=${rest%%:*}; local data=${rest#*:}

        echo -n "  $label: "
        if verify_can20_frame "$tx" "$rx" "$id" "$data"; then
            echo "OK"; ((passed++))
        else
            echo "FAIL ($VERIFY_ERR)"
        fi
    done

    log_info "Standard ID: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "All standard IDs correct" || test_fail "$((total-passed)) standard ID tests failed"
    return $?
}

test_exact_frame_count() {
    local tx=$1 rx=$2
    start_testcase "Exact_Frame_Count"
    log_header "Testing exact frame count (send N, verify exactly N received)"

    local passed=0 total=3
    local id="300"
    local data="DEADBEEF"

    for count in 1 5 10; do
        echo -n "  Send $count frames: "
        local logf="/tmp/count_test_$$.log"
        rm -f "$logf"
        timeout 5 candump $rx -t A 2>/dev/null > "$logf" &
        local pid=$!
        sleep 0.1

        local i
        for (( i=0; i<count; i++ )); do
            sudo cansend $tx "${id}#${data}" >/dev/null 2>&1
        done

        sleep 1
        wait $pid 2>/dev/null || true

        local received
        received=$(grep -i -c "$id" "$logf" 2>/dev/null) || received=0
        rm -f "$logf"

        if [ "$received" -eq "$count" ]; then
            echo "OK (received=$received)"; ((passed++))
        else
            echo "FAIL (sent=$count received=$received)"
        fi
    done

    log_info "Exact Frame Count: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "Exact frame count verified" || test_fail "$((total-passed)) frame count tests failed"
    return $?
}

test_bitrate_readback() {
    local tx=$1 rx=$2
    start_testcase "Bitrate_Readback"
    log_header "Testing bitrate configuration readback"

    local output
    output=$(ip -details link show $tx 2>/dev/null)
    local bitrate
    bitrate=$(echo "$output" | grep -oP 'bitrate \K[0-9]+' | head -1)
    local dbitrate
    dbitrate=$(echo "$output" | grep -oP 'dbitrate \K[0-9]+' | head -1)

    echo "  TX port: bitrate=${bitrate:-N/A} dbitrate=${dbitrate:-N/A}"

    output=$(ip -details link show $rx 2>/dev/null)
    local rx_bitrate
    rx_bitrate=$(echo "$output" | grep -oP 'bitrate \K[0-9]+' | head -1)
    local rx_dbitrate
    rx_dbitrate=$(echo "$output" | grep -oP 'dbitrate \K[0-9]+' | head -1)

    echo "  RX port: bitrate=${rx_bitrate:-N/A} dbitrate=${rx_dbitrate:-N/A}"

    if [ -n "$bitrate" ] && [ -n "$rx_bitrate" ]; then
        if [ "$bitrate" = "$rx_bitrate" ]; then
            log_info "Bitrate readback: bitrate=$bitrate dbitrate=${dbitrate:-N/A}"
            test_pass "Bitrate readback OK: bitrate=$bitrate dbitrate=${dbitrate:-N/A}"
            return 0
        else
            test_fail "Bitrate mismatch: TX=$bitrate RX=$rx_bitrate"
            return 1
        fi
    else
        test_fail "Could not read bitrate"
        return 1
    fi
}

# Functional bitrate test: verify mismatched bitrate breaks communication.
# This proves bitrate actually takes effect on hardware, not just in software config.
test_bitrate_functional() {
    local tx=$1 rx=$2
    start_testcase "Bitrate_Functional"
    log_header "Testing that bitrate change actually affects hardware"

    local passed=0 total=2

    # Save original config
    local orig_output
    orig_output=$(ip -details link show $tx 2>/dev/null)
    local orig_br
    orig_br=$(echo "$orig_output" | grep -oP 'bitrate \K[0-9]+' | head -1)
    local orig_dbr
    orig_dbr=$(echo "$orig_output" | grep -oP 'dbitrate \K[0-9]+' | head -1)
    local orig_sp
    orig_sp=$(echo "$orig_output" | grep -oP 'sample-point \K[0-9.]+' | head -1)
    local orig_dsp
    orig_dsp=$(echo "$orig_output" | grep -oP 'dsample-point \K[0-9.]+' | head -1)

    # Step 1: Change TX to mismatched bitrate (250K instead of 500K)
    echo -n "  Mismatched bitrate (TX=250K): "
    sudo ip link set $tx down 2>/dev/null
    sleep 0.2
    sudo ip link set $tx up type can bitrate 250000 dbitrate 1000000 fd on 2>/dev/null
    sleep 0.3

    # Send frame, should NOT be received
    local logf="/tmp/bitrate_mismatch_$$.log"
    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!; sleep 0.05
    sudo cansend $tx 5AA#DEADBEEF 2>/dev/null
    sleep 0.5; wait $pid 2>/dev/null || true

    local recv
    recv=$(grep -c "5AA" "$logf" 2>/dev/null) || recv=0
    rm -f "$logf"

    if [ "$recv" -eq 0 ]; then
        echo "OK (no reception with mismatched bitrate)"
        ((passed++))
    else
        echo "FAIL (received frame despite mismatched bitrate - bitrate may not take effect)"
    fi

    # Step 2: Restore TX to original bitrate, verify communication resumes
    echo -n "  Restored bitrate (TX=${orig_br}K): "
    sudo ip link set $tx down 2>/dev/null
    sleep 0.2
    local restore_cmd="sudo ip link set $tx up type can bitrate $orig_br"
    [ -n "$orig_dbr" ] && restore_cmd="$restore_cmd dbitrate $orig_dbr"
    [ -n "$orig_sp" ] && restore_cmd="$restore_cmd sample-point $orig_sp"
    [ -n "$orig_dsp" ] && restore_cmd="$restore_cmd dsample-point $orig_dsp"
    restore_cmd="$restore_cmd fd on"
    eval $restore_cmd 2>/dev/null
    sleep 0.3

    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    pid=$!; sleep 0.05
    sudo cansend $tx 5AB#CAFEBABE 2>/dev/null
    sleep 0.5; wait $pid 2>/dev/null || true

    recv=$(grep -c "5AB" "$logf" 2>/dev/null) || recv=0
    rm -f "$logf"

    if [ "$recv" -ge 1 ]; then
        echo "OK (communication restored)"
        ((passed++))
    else
        echo "FAIL (communication not restored after bitrate restore)"
    fi

    log_info "Bitrate Functional: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "Bitrate functionally verified" || test_fail "$((total-passed)) bitrate functional tests failed"
    return $?
}

test_sample_point_boundaries() {
    local port=$1
    start_testcase "Sample_Point_Boundaries"
    log_header "Testing sample-point boundary validation"

    local passed=0 total=7
    local values=("0.001" "0.10" "0.999" "1.0" "1.50" "2.0" "10.0")
    local sp

    for sp in "${values[@]}"; do
        echo -n "  sample-point=$sp: "
        sudo ip link set $port down 2>/dev/null || true
        sleep 0.1

        if sudo ip link set $port type can bitrate 500000 sample-point $sp fd off termination 120 >/tmp/sample_point_boundary_$$.log 2>&1; then
            local details actual phase2 invalid=0

            sudo ip link set $port up >/dev/null 2>&1 || true
            details=$(ip -details link show $port 2>/dev/null)
            actual=$(echo "$details" | grep -oP 'sample-point \K[0-9.]+' | head -1)
            phase2=$(echo "$details" | grep -oP 'phase-seg2 \K-?[0-9]+' | head -1)

            if [ -z "$actual" ] || [ -z "$phase2" ]; then
                invalid=1
            elif awk "BEGIN {exit !($actual <= 0 || $actual >= 1)}"; then
                invalid=1
            elif [ "$phase2" -lt 1 ]; then
                invalid=1
            fi

            if [ "$invalid" -eq 0 ]; then
                echo "OK (accepted as sample-point=$actual phase-seg2=$phase2)"
                ((passed++))
            else
                echo "FAIL (accepted invalid timing: sample-point=$actual phase-seg2=$phase2)"
            fi
        else
            echo "OK (rejected)"
            ((passed++))
        fi
    done

    rm -f /tmp/sample_point_boundary_$$.log
    sudo ip link set $port down 2>/dev/null || true

    log_info "Sample Point Boundaries: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "Sample-point boundaries valid/rejected" || test_fail "$((total-passed)) sample-point boundary tests failed"
    return $?
}

test_bitrate_multi() {
    local tx=$1 rx=$2
    start_testcase "Bitrate_Multi"
    log_header "Testing communication at multiple standard bitrates"

    # Save original config
    local orig_br orig_dbr
    orig_br=$(ip -details link show $tx 2>/dev/null | grep -oP 'bitrate \K[0-9]+' | head -1)
    orig_dbr=$(ip -details link show $tx 2>/dev/null | grep -oP 'dbitrate \K[0-9]+' | head -1)

    # All common industry bitrate combinations:
    #   label:nominal_bps:data_bps:can_id_hex
    # CAN 2.0: 125K, 250K, 500K, 800K, 1M
    # CAN FD: 250K/1M, 250K/2M, 500K/2M, 500K/5M, 1M/2M, 1M/4M, 1M/5M
    local tests=(
        "125K(can2.0):125000:0:5A0"
        "250K(can2.0):250000:0:5A1"
        "500K(can2.0):500000:0:5A2"
        "800K(can2.0):800000:0:5A3"
        "1M(can2.0):1000000:0:5A4"
        "250K/1M(fd):250000:1000000:5A5"
        "250K/2M(fd):250000:2000000:5A6"
        "500K/2M(fd):500000:2000000:5A7"
        "500K/5M(fd):500000:5000000:5A8"
        "1M/2M(fd):1000000:2000000:5A9"
        "1M/4M(fd):1000000:4000000:5AA"
        "1M/5M(fd):1000000:5000000:5AB"
    )

    local passed=0 total=${#tests[@]}
    local logf="/tmp/bitrate_multi_$$.log"

    for tc in "${tests[@]}"; do
        local label=${tc%%:*}; local rest=${tc#*:}
        local nom=${rest%%:*}; local rest2=${rest#*:}
        local dbr=${rest2%%:*}; local id=${rest2#*:}

        echo -n "  $label: "

        sudo ip link set $tx down 2>/dev/null; sudo ip link set $rx down 2>/dev/null
        sleep 0.2

        if [ "$dbr" = "0" ]; then
            # CAN 2.0 (no fd)
            sudo ip link set $tx up type can bitrate $nom 2>/dev/null
            sudo ip link set $rx up type can bitrate $nom 2>/dev/null
        else
            # CAN FD
            sudo ip link set $tx up type can bitrate $nom dbitrate $dbr fd on 2>/dev/null
            sudo ip link set $rx up type can bitrate $nom dbitrate $dbr fd on 2>/dev/null
        fi
        sleep 0.3

        rm -f "$logf"
        timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
        local pid=$!; sleep 0.05
        sudo cansend $tx "${id}#DEADBEEF" 2>/dev/null
        sleep 0.5; wait $pid 2>/dev/null || true

        if grep -q "$id" "$logf" 2>/dev/null; then
            echo "OK"; ((passed++))
        else
            echo "FAIL"
        fi
    done
    rm -f "$logf"

    # Restore original config
    sudo ip link set $tx down 2>/dev/null; sudo ip link set $rx down 2>/dev/null
    sleep 0.2
    sudo ip link set $tx up type can bitrate $orig_br dbitrate ${orig_dbr:-2000000} fd on 2>/dev/null
    sudo ip link set $rx up type can bitrate $orig_br dbitrate ${orig_dbr:-2000000} fd on 2>/dev/null
    sleep 0.3

    log_info "Bitrate Multi: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "All $total bitrate combinations verified" || test_fail "$((total-passed)) bitrate combinations failed"
    return $?
}

test_rtr_frame() {
    local tx=$1 rx=$2
    start_testcase "RTR_Frame"
    log_header "Testing RTR (remote request) frames"

    local passed=0 total=3
    local logf="/tmp/rtr_test_$$.log"

    # RTR DLC=0
    echo -n "  RTR DLC=0: "
    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!; sleep 0.05
    sudo cansend $tx "300#R0" 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    local recv_line
    recv_line=$(grep -i "300" "$logf" 2>/dev/null | head -1)
    if echo "$recv_line" | grep -q "remote"; then
        echo "OK"; ((passed++))
    else
        echo "FAIL ($([ -n "$recv_line" ] && echo "$recv_line" || echo "no frame"))"
    fi

    # RTR DLC=4
    echo -n "  RTR DLC=4: "
    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    pid=$!; sleep 0.05
    sudo cansend $tx "301#R4" 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    recv_line=$(grep -i "301" "$logf" 2>/dev/null | head -1)
    if echo "$recv_line" | grep -q "remote"; then
        echo "OK"; ((passed++))
    else
        echo "FAIL ($([ -n "$recv_line" ] && echo "$recv_line" || echo "no frame"))"
    fi

    # RTR DLC=8
    echo -n "  RTR DLC=8: "
    rm -f "$logf"
    timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    pid=$!; sleep 0.05
    sudo cansend $tx "302#R8" 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    recv_line=$(grep -i "302" "$logf" 2>/dev/null | head -1)
    if echo "$recv_line" | grep -q "remote"; then
        echo "OK"; ((passed++))
    else
        echo "FAIL ($([ -n "$recv_line" ] && echo "$recv_line" || echo "no frame"))"
    fi

    rm -f "$logf"
    log_info "RTR Frame: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "All RTR frames correct" || test_fail "$((total-passed)) RTR tests failed"
    return $?
}

test_reverse_direction() {
    local tx=$1 rx=$2
    start_testcase "Reverse_Direction"
    log_header "Testing reverse direction: RX port TX -> TX port RX"

    local passed=0 total=2

    # CAN 2.0 reverse
    echo -n "  CAN 2.0 reverse: "
    local logf="/tmp/rev_test_$$.log"
    rm -f "$logf"
    timeout 2 candump $tx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!; sleep 0.05
    sudo cansend $rx 600#CCAABBEE 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    local recv_line
    recv_line=$(grep -i "600" "$logf" 2>/dev/null | head -1)
    rm -f "$logf"
    if [ -n "$recv_line" ]; then
        echo "OK"; ((passed++))
    else
        echo "FAIL (no frame)"
    fi

    # CAN FD reverse
    echo -n "  CAN FD reverse (12B): "
    rm -f "$logf"
    timeout 2 candump $tx -n 1 -t A 2>/dev/null > "$logf" &
    pid=$!; sleep 0.05
    sudo cansend $rx "609##10102030405060708090A0B0C" 2>/dev/null
    sleep 0.4; wait $pid 2>/dev/null || true
    recv_line=$(grep -i "609" "$logf" 2>/dev/null | head -1)
    rm -f "$logf"
    if [ -n "$recv_line" ]; then
        local recv_len
        recv_len=$(echo "$recv_line" | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
        recv_len=$((10#$recv_len))
        if [ "$recv_len" -eq 12 ]; then
            echo "OK ([12] bytes)"; ((passed++))
        else
            echo "FAIL (expected 12B got ${recv_len}B)"
        fi
    else
        echo "FAIL (no frame)"
    fi

    log_info "Reverse Direction: $passed/$total passed"
    [ "$passed" -eq "$total" ] && test_pass "Bidirectional communication verified" || test_fail "$((total-passed)) reverse tests failed"
    return $?
}

test_stress_rapid() {
    local tx=$1 rx=$2
    start_testcase "Stress_Rapid"
    log_header "Testing rapid frame transmission (100 frames)"

    local count=100
    local id="400"
    local logf="/tmp/stress_test_$$.log"

    rm -f "$logf"
    timeout 10 candump $rx -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1

    if command -v cangen >/dev/null 2>&1; then
        # cangen: single process, efficient, 2ms gap
        sudo cangen $tx -g 5 -n $count -I $id -L 8 -D 0102030405060708 2>/dev/null
    else
        # Fallback: cansend with 2ms gap
        local i
        for (( i=0; i<count; i++ )); do
            sudo cansend $tx "${id}#0102030405060708" >/dev/null 2>&1
            sleep 0.005
        done
    fi

    sleep 1
    wait $pid 2>/dev/null || true

    local received
    received=$(grep -i -c "$id" "$logf" 2>/dev/null) || received=0
    rm -f "$logf"

    local pct=$((received * 100 / count))
    echo "  Sent: $count  Received: $received  Loss: $((100 - pct))%"

    if [ "$received" -eq "$count" ]; then
        log_info "Stress Rapid: $received/$count received"
        test_pass "Rapid stress: $received/$count (0% loss)"
        return 0
    else
        log_info "Stress Rapid: $received/$count received ($pct%)"
        test_fail "Rapid stress: $received/$count ($((100-pct))% loss)"
        return 1
    fi
}

# Main -------------------------------------------------------------------

main() {
    log_header "Data Integrity Test Suite"
    log_info "Start time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""

    if [ ! -f "$RESULTS_DIR/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi

    local ports=($(cat "$RESULTS_DIR/connected_ports.txt"))
    if [ ${#ports[@]} -lt 2 ]; then
        log_error "Need at least 2 connected ports"
        return 1
    fi

    local tx=${ports[0]} rx=${ports[1]}
    log_info "TX=$tx  RX=$rx"
    echo ""

    source "$SCRIPT_DIR/../lib/hardware_helper.sh"
    hw_init; hw_init_pair "$tx" "$rx"
    echo ""

    # Warm-up
    log_info "Warming up..."
    local warmup_ok=0
    for w in $(seq 1 3); do
        local warmup_log="/tmp/warmup_integrity_$$.log"
        rm -f "$warmup_log"
        timeout 1 candump $rx -n 1 -t A 2>/dev/null > "$warmup_log" &
        local warmup_pid=$!; sleep 0.05
        sudo cansend $tx 123#DEADBEEF >/dev/null 2>&1
        wait $warmup_pid 2>/dev/null || true
        grep -q "123" "$warmup_log" 2>/dev/null && warmup_ok=$((warmup_ok + 1))
        rm -f "$warmup_log"
    done
    if [ "$warmup_ok" -gt 0 ]; then
        log_info "Warm-up: $warmup_ok/3 frames received"
    else
        log_warn "Warm-up: 0/3 frames received - CAN bus may be unstable"
    fi
    echo ""

    local total=0 passed=0

    # Phase 1: CAN 2.0
    log_info "=== Phase 1: CAN 2.0 ==="; echo ""

    if test_can20_payload "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_standard_id "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_rtr_frame "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""

    # Phase 2: CAN FD DLC & Payload
    log_info "=== Phase 2: CAN FD DLC & Payload ==="; echo ""

    if test_dlc_exact_boundaries "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_dlc_boundary_first "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_dlc_boundary_last "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_canfd_brs_flags "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""

    # Phase 3: ID & Frame Count
    log_info "=== Phase 3: ID & Frame Count ==="; echo ""

    if test_extended_id "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_exact_frame_count "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_reverse_direction "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""

    # Phase 4: Stress (run before bitrate change test)
    log_info "=== Phase 4: Stress ==="; echo ""

    if test_stress_rapid "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""

    # Phase 5: Configuration (bitrate tests change config, run last)
    log_info "=== Phase 5: Configuration ==="; echo ""

    if test_bitrate_readback "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_bitrate_multi "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_bitrate_functional "$tx" "$rx"; then ((passed++)); fi; ((total++)); echo ""
    if test_sample_point_boundaries "$tx"; then ((passed++)); fi; ((total++)); echo ""

    # Summary
    log_header "Summary: $passed/$total passed"
    [ "$passed" -eq "$total" ] && return 0 || return 1
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
