#!/bin/bash
# Module: bm_doc_commands.sh
# Validates every command-line example from project documentation:
#   - README.md
#   - doc/USER_MANUAL.md
#   - doc/QUICK_START.md
#   - doc/TEST_GUIDE.md
#
# When updating .md files with command-line changes, update this module too.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

append_testsuite

# Port pair (from connected_ports.txt or fallback)
DOC_TX=""
DOC_RX=""

################################################################################
# Helpers
################################################################################

# Run a command, expect exit code 0. Log PASS/FAIL.
doc_cmd() {
    local name=$1
    shift
    local cmd=$*
    local rc=0
    eval "$cmd" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        log_success "  $name: OK"
        return 0
    else
        log_fail "  $name: FAIL (rc=$rc)"
        return 1
    fi
}

# Send frame on TX port, verify RX port receives within timeout.
# Usage: doc_send_recv <name> <can_id> <cansend_args>
doc_send_recv() {
    local name=$1
    local can_id=$2
    shift 2
    local frame=$*

    local logf="/tmp/doc_cmd_$$.log"
    local id_hex=$(echo "$can_id" | sed 's/0x//g' | tr '[:upper:]' '[:lower:]')

    timeout 2 candump $DOC_RX -n 1 -t A > "$logf" 2>/dev/null &
    local pid=$!
    sleep 0.1

    sudo cansend $DOC_TX "$frame" >/dev/null 2>&1
    sleep 0.3

    wait $pid 2>/dev/null || true
    local received
    received=$(grep -i -c "$id_hex" "$logf" 2>/dev/null) || received=0
    rm -f "$logf"

    if [ "$received" -gt 0 ]; then
        log_success "  $name: OK"
        return 0
    else
        log_fail "  $name: FAIL (no frame received)"
        return 1
    fi
}

################################################################################
# Category 1: System/Build Commands
# Sources: README.md, QUICK_START.md, TEST_GUIDE.md
################################################################################

test_system_commands() {
    local passed=0
    local total=0

    # README.md / QUICK_START.md / TEST_GUIDE.md: "lsmod | grep bmcan"
    ((total++))
    doc_cmd "Sys_lsmod_bmcan" "lsmod | grep -q bmcan" && ((passed++))

    # README.md / QUICK_START.md / TEST_GUIDE.md: "ip link show type can"
    ((total++))
    doc_cmd "Sys_ip_link_show_can" "ip link show type can | grep -q can" && ((passed++))

    # QUICK_START.md / TEST_GUIDE.md: "ip -details link show can0"
    ((total++))
    doc_cmd "Sys_ip_details_can0" "ip -details link show $DOC_TX | grep -q can" && ((passed++))

    # QUICK_START.md: "ip -details link show can0 | grep termination"
    ((total++))
    doc_cmd "Sys_ip_details_termination" "ip -details link show $DOC_TX | grep -q termination" && ((passed++))

    # QUICK_START.md: "ip -details link show can0 | grep mtu"
    ((total++))
    doc_cmd "Sys_ip_details_mtu" "ip -details link show $DOC_TX | grep -q mtu" && ((passed++))

    # QUICK_START.md / TEST_GUIDE.md: "lsusb | grep 0810"
    ((total++))
    doc_cmd "Sys_lsusb_busmust" "lsusb | grep -q 0810" && ((passed++))

    # QUICK_START.md / TEST_GUIDE.md: "sudo dmesg | tail -30"
    ((total++))
    doc_cmd "Sys_dmesg_tail" "sudo dmesg | tail -30 >/dev/null" && ((passed++))

    # README.md / QUICK_START.md: "sudo ip link set can0 down"
    ((total++))
    sudo ip link set $DOC_TX down 2>/dev/null
    sudo ip link set $DOC_TX up type can bitrate 500000 dbitrate 2000000 fd on termination 120 >/dev/null 2>&1
    sleep 0.3
    doc_cmd "Sys_ip_link_down_up" "ip link show $DOC_TX | grep -q UP" && ((passed++))

    # README.md: "sudo ip link set can0 type can termination 0"
    ((total++))
    sudo ip link set $DOC_TX type can termination 0 >/dev/null 2>&1
    local term_off=$(ip -details link show $DOC_TX | grep termination | grep -c "0 " || true)
    sudo ip link set $DOC_TX type can termination 120 >/dev/null 2>&1
    if [ "$term_off" -gt 0 ]; then
        log_success "  Sys_ip_termination_0: OK"
        ((passed++))
    else
        log_fail "  Sys_ip_termination_0: FAIL"
    fi

    # README.md: "sudo ip link set can0 type can termination 120"
    ((total++))
    sudo ip link set $DOC_TX type can termination 120 >/dev/null 2>&1
    local term_on=$(ip -details link show $DOC_TX | grep -c "termination 120" || true)
    if [ "$term_on" -gt 0 ]; then
        log_success "  Sys_ip_termination_120: OK"
        ((passed++))
    else
        log_fail "  Sys_ip_termination_120: FAIL"
    fi

    log_info "System commands: $passed/$total passed"
    return $((total - passed))
}

################################################################################
# Category 2: CAN Frame Commands
# Sources: README.md, QUICK_START.md, TEST_GUIDE.md
################################################################################

test_can_frame_commands() {
    local passed=0
    local total=0

    # Ensure ports are up
    sudo ip link set $DOC_TX up type can bitrate 500000 dbitrate 2000000 fd on termination 120 >/dev/null 2>&1
    sudo ip link set $DOC_RX up type can bitrate 500000 dbitrate 2000000 fd on termination 120 >/dev/null 2>&1
    sleep 0.5

    # README.md / QUICK_START.md: "cansend can0 123#DEADBEEF" (CAN 2.0 std)
    ((total++))
    doc_send_recv "Frame_CAN20_Std_123_DEADBEEF" "123" "123#DEADBEEF" && ((passed++))

    # QUICK_START.md: "cansend can0 12345678#DEADBEEF" (CAN 2.0 ext)
    ((total++))
    doc_send_recv "Frame_CAN20_Ext_12345678" "12345678" "12345678#DEADBEEF" && ((passed++))

    # QUICK_START.md: "cansend can0 123##0DEADBEEF" (CAN FD std)
    ((total++))
    doc_send_recv "Frame_CANFD_Std_123" "123" "123##0DEADBEEF" && ((passed++))

    # QUICK_START.md: "cansend can0 12345678##0DEADBEEF" (CAN FD ext)
    ((total++))
    doc_send_recv "Frame_CANFD_Ext_12345678" "12345678" "12345678##0DEADBEEF" && ((passed++))

    # QUICK_START.md / TEST_GUIDE.md: CAN FD 64-byte frame
    ((total++))
    doc_send_recv "Frame_CANFD_64byte" "123" \
        "123##00102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F40" && ((passed++))

    log_info "CAN frame commands: $passed/$total passed"
    return $((total - passed))
}

################################################################################
# Category 3: API Tool Commands
# Sources: USER_MANUAL.md, TEST_GUIDE.md
################################################################################

test_api_commands() {
    local passed=0
    local total=0
    local gen=$(detect_generation)

    if [ ! -x "$API_BIN" ]; then
        log_error "API tool not found: $API_BIN"
        return 1
    fi

    # USER_MANUAL.md: txtasks --type fixed
    ((total++))
    sudo $API_BIN txtasks --type fixed --id 0x123 --payload DEADBEEF --cycle 100 \
        --index 0 --device $DOC_TX --apply >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_txtasks_fixed: OK"
        ((passed++))
    else
        log_fail "  API_txtasks_fixed: FAIL"
    fi

    # USER_MANUAL.md: txtasks --type incdata
    ((total++))
    sudo $API_BIN txtasks --type incdata --id 0x200 --payload 00000000 --cycle 50 \
        --index 1 --device $DOC_TX --apply >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_txtasks_incdata: OK"
        ((passed++))
    else
        log_fail "  API_txtasks_incdata: FAIL"
    fi

    # USER_MANUAL.md: txtasks --type incid
    ((total++))
    sudo $API_BIN txtasks --type incid --id 0x100 --payload 12345678 --cycle 100 \
        --index 2 --device $DOC_TX --apply >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_txtasks_incid: OK"
        ((passed++))
    else
        log_fail "  API_txtasks_incid: FAIL"
    fi

    # USER_MANUAL.md: invalidate --txtasks --tx-range 0-0
    ((total++))
    sudo $API_BIN invalidate --txtasks --tx-range 0-0 --device $DOC_TX >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_invalidate_txtasks_single: OK"
        ((passed++))
    else
        log_fail "  API_invalidate_txtasks_single: FAIL"
    fi

    # USER_MANUAL.md: invalidate --txtasks --tx-range 0-63
    ((total++))
    sudo $API_BIN invalidate --txtasks --tx-range 0-63 --device $DOC_TX >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_invalidate_txtasks_all: OK"
        ((passed++))
    else
        log_fail "  API_invalidate_txtasks_all: FAIL"
    fi

    # USER_MANUAL.md: routes --route-type broadcast
    ((total++))
    sudo $API_BIN routes --route-type broadcast --source 0 --target 0xFF \
        --index 1 --device $DOC_TX --apply >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_routes_broadcast: OK"
        ((passed++))
    else
        log_fail "  API_routes_broadcast: FAIL"
    fi

    # USER_MANUAL.md: routes --route-type unicast with ID filter
    ((total++))
    sudo $API_BIN routes --route-type unicast --source 0 --target 1 \
        --id-value 0x123 --id-mask 0x7FF --index 0 \
        --device $DOC_TX --apply >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_routes_unicast_filter: OK"
        ((passed++))
    else
        log_fail "  API_routes_unicast_filter: FAIL"
    fi

    # USER_MANUAL.md: invalidate --route --route-range 0-63
    ((total++))
    sudo $API_BIN invalidate --route --route-range 0-63 --device $DOC_TX >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_invalidate_routes: OK"
        ((passed++))
    else
        log_fail "  API_invalidate_routes: FAIL"
    fi

    # USER_MANUAL.md: invalidate --all
    ((total++))
    sudo $API_BIN invalidate --all --device $DOC_TX >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  API_invalidate_all: OK"
        ((passed++))
    else
        log_fail "  API_invalidate_all: FAIL"
    fi

    # USER_MANUAL.md: save (Gen2.5+ only)
    if [ "$gen" = "gen25" ] || [ "$gen" = "gen3" ]; then
        ((total++))
        sudo $API_BIN save --device $DOC_TX >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_save: OK"
            ((passed++))
        else
            log_fail "  API_save: FAIL"
        fi

        # USER_MANUAL.md: load
        ((total++))
        sudo $API_BIN load --device $DOC_TX >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_load: OK"
            ((passed++))
        else
            log_fail "  API_load: FAIL"
        fi

        # USER_MANUAL.md: clear
        ((total++))
        sudo $API_BIN clear --device $DOC_TX >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_clear: OK"
            ((passed++))
        else
            log_fail "  API_clear: FAIL"
        fi
    else
        log_info "  Skipping save/load/clear (Gen2.5+ only)"
    fi

    # USER_MANUAL.md: logging (Gen2.5 only)
    if [ "$gen" = "gen25" ]; then
        ((total++))
        sudo $API_BIN logging --mode always --format bbd --channels 0xFFFF \
            --device $DOC_TX --apply >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_logging_start: OK"
            ((passed++))
        else
            log_fail "  API_logging_start: FAIL"
        fi

        ((total++))
        sudo $API_BIN logging --mode off --device $DOC_TX --apply >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_logging_stop: OK"
            ((passed++))
        else
            log_fail "  API_logging_stop: FAIL"
        fi

        # USER_MANUAL.md: replay (Gen2.5 only)
        ((total++))
        sudo $API_BIN replay --mode always --format bbd --channels 0xFFFF \
            --device $DOC_TX --apply >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_replay_start: OK"
            ((passed++))
        else
            log_fail "  API_replay_start: FAIL"
        fi

        ((total++))
        sudo $API_BIN replay --mode off --device $DOC_TX --apply >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  API_replay_stop: OK"
            ((passed++))
        else
            log_fail "  API_replay_stop: FAIL"
        fi
    else
        log_info "  Skipping logging/replay (Gen2.5 only)"
    fi

    log_info "API commands: $passed/$total passed"
    return $((total - passed))
}

################################################################################
# Category 4: sysfs Interface Commands
# Source: USER_MANUAL.md
################################################################################

test_sysfs_commands() {
    local passed=0
    local total=0
    local gen=$(detect_generation)

    # USER_MANUAL.md: echo hex blob > txtasks
    ((total++))
    echo '0 0101000000000000640000010000000001230000000000000000000000000000DEADBEEF0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000' \
        | sudo tee /sys/class/net/$DOC_TX/txtasks >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  SysFs_txtasks_write: OK"
        ((passed++))
    else
        log_fail "  SysFs_txtasks_write: FAIL"
    fi

    # USER_MANUAL.md: cat txtasks
    ((total++))
    sudo cat /sys/class/net/$DOC_TX/txtasks >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  SysFs_txtasks_read: OK"
        ((passed++))
    else
        log_fail "  SysFs_txtasks_read: FAIL"
    fi

    # USER_MANUAL.md: echo hex blob > routes (broadcast)
    ((total++))
    echo '0 010001000100000000000000FF000000' \
        | sudo tee /sys/class/net/$DOC_TX/routes >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  SysFs_routes_write: OK"
        ((passed++))
    else
        log_fail "  SysFs_routes_write: FAIL"
    fi

    # USER_MANUAL.md: cat routes
    ((total++))
    sudo cat /sys/class/net/$DOC_TX/routes >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_success "  SysFs_routes_read: OK"
        ((passed++))
    else
        log_fail "  SysFs_routes_read: FAIL"
    fi

    # USER_MANUAL.md: save/load/clear via sysfs (Gen2.5+ only)
    if [ "$gen" = "gen25" ] || [ "$gen" = "gen3" ]; then
        ((total++))
        echo '0400' | sudo tee /sys/class/net/$DOC_TX/save >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  SysFs_save: OK"
            ((passed++))
        else
            log_fail "  SysFs_save: FAIL"
        fi

        ((total++))
        echo '0600' | sudo tee /sys/class/net/$DOC_TX/load >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            log_success "  SysFs_load: OK"
            ((passed++))
        else
            log_fail "  SysFs_load: FAIL"
        fi

        ((total++))
        echo 'FFFF' | sudo tee /sys/class/net/$DOC_TX/clear >/dev/null 2>&1
        # tee may return 0 even on write error; check dmesg for errors
        if ! sudo dmesg | tail -3 | grep -qi "error\|failed\|invalid" 2>/dev/null; then
            log_success "  SysFs_clear: OK"
            ((passed++))
        else
            log_fail "  SysFs_clear: FAIL"
        fi
    else
        log_info "  Skipping sysfs save/load/clear (Gen2.5+ only)"
    fi

    # Cleanup sysfs state
    sudo $API_BIN invalidate --all --device $DOC_TX >/dev/null 2>&1 || true

    log_info "sysfs commands: $passed/$total passed"
    return $((total - passed))
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Documentation Command Validation"
    log_info "=========================================="
    echo ""

    # Read port pair
    if [ -f "$CONNECTED_PORTS_FILE" ]; then
        local ports=($(cat "$CONNECTED_PORTS_FILE"))
        DOC_TX=${ports[0]}
        DOC_RX=${ports[1]}
    fi

    if [ -z "$DOC_TX" ] || [ -z "$DOC_RX" ]; then
        log_error "No port pair available"
        return 1
    fi

    log_info "TX port: $DOC_TX"
    log_info "RX port: $DOC_RX"
    log_info "Generation: $(detect_generation)"
    echo ""

    local total_failures=0

    # Test 1: System commands
    log_header "System Commands (README.md, QUICK_START.md, TEST_GUIDE.md)"
    start_testcase "DocCmd_System"
    local sys_fail
    test_system_commands
    sys_fail=$?
    if [ "$sys_fail" -eq 0 ]; then
        test_pass "All system commands OK"
    else
        test_fail "$sys_fail system command(s) failed"
        ((total_failures += sys_fail))
    fi
    echo ""

    # Test 2: CAN frame commands
    log_header "CAN Frame Commands (README.md, QUICK_START.md, TEST_GUIDE.md)"
    start_testcase "DocCmd_CAN_Frames"
    local frame_fail
    test_can_frame_commands
    frame_fail=$?
    if [ "$frame_fail" -eq 0 ]; then
        test_pass "All CAN frame commands OK"
    else
        test_fail "$frame_fail CAN frame command(s) failed"
        ((total_failures += frame_fail))
    fi
    echo ""

    # Test 3: API tool commands
    log_header "API Tool Commands (USER_MANUAL.md, TEST_GUIDE.md)"
    start_testcase "DocCmd_API_Tool"
    local api_fail
    test_api_commands
    api_fail=$?
    if [ "$api_fail" -eq 0 ]; then
        test_pass "All API commands OK"
    else
        test_fail "$api_fail API command(s) failed"
        ((total_failures += api_fail))
    fi
    echo ""

    # Test 4: sysfs interface commands
    log_header "sysfs Interface Commands (USER_MANUAL.md)"
    start_testcase "DocCmd_SysFs"
    local sysfs_fail
    test_sysfs_commands
    sysfs_fail=$?
    if [ "$sysfs_fail" -eq 0 ]; then
        test_pass "All sysfs commands OK"
    else
        test_fail "$sysfs_fail sysfs command(s) failed"
        ((total_failures += sysfs_fail))
    fi
    echo ""

    if [ "$total_failures" -eq 0 ]; then
        log_success "All documentation commands validated"
    else
        log_fail "$total_failures command(s) failed"
    fi

    return $total_failures
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
