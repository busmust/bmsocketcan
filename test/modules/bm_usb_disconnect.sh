#!/bin/bash
# Module: USB Disconnect Test
#
# Tests that USB device removal:
# - Kernel remains stable (no oops/panic)
# - All CAN ports for that device disappear from ip link
# - Re-plug device restores all ports
# - No resource leaks after multiple disconnect/reconnect cycles
#
# NOTE: This test requires physical USB plug/unplug or USB port power cycle.
#       In automated environments, use usbreset or bind/unbind as substitute.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"

################################################################################
# Helpers
################################################################################

# Count currently visible CAN interfaces
count_can_interfaces() {
    ip link show 2>/dev/null | grep -c ': can[0-9]*:' || echo 0
}

# Get list of CAN interfaces
get_can_interfaces() {
    ip -o link show 2>/dev/null | grep ': can[0-9]*:' | sed 's/.*: \(can[0-9]*\):.*/\1/' | sort
}

# Check kernel is healthy (no oops/panic in dmesg since baseline)
check_kernel_health() {
    local baseline=${1:-0}
    if ! check_new_oops "$baseline"; then
        log_error "Kernel instability detected"
        return 1
    fi
    return 0
}

# Check module is loaded
check_module_loaded() {
    lsmod | grep -q bmcan
}

################################################################################
# Test Cases
################################################################################

test_ports_disappear_on_disconnect() {
    local before_count=$1
    local after_count=$2

    if [ "$after_count" -lt "$before_count" ]; then
        log_success "Ports disappeared: $before_count -> $after_count"
        return 0
    else
        log_fail "Ports did not disappear: $before_count -> $after_count"
        return 1
    fi
}

test_ports_reappear_on_reconnect() {
    local original_count=$1
    local current_count=$2

    if [ "$current_count" -ge "$original_count" ]; then
        log_success "Ports restored: $current_count interfaces available"
        return 0
    else
        log_fail "Ports not fully restored: $current_count < $original_count"
        return 1
    fi
}

test_no_resource_leak() {
    # Compare interface count before and after a disconnect/reconnect cycle
    local before=$1
    local after=$2

    if [ "$after" -eq "$before" ]; then
        log_success "No resource leak: interface count matches ($before)"
        return 0
    else
        log_fail "Possible resource leak: before=$before after=$after"
        return 1
    fi
}

test_communication_after_reconnect() {
    local tx=$1 rx=$2

    # Re-init ports with extended settle time after USB re-enumeration
    sudo ip link set $tx down 2>/dev/null || true
    sudo ip link set $rx down 2>/dev/null || true
    sleep 1
    sudo ip link set $tx up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
    sudo ip link set $rx up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
    sleep 2

    # Test communication
    local logf="/tmp/usb_reconnect_comm_$$.log"
    timeout 3 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.3
    sudo cansend $tx 0FF#AABBCCDD >/dev/null 2>&1
    wait $pid 2>/dev/null || true

    local recv; recv=$(grep -c "0FF" "$logf" 2>/dev/null) || recv=0
    rm -f "$logf"

    if [ "$recv" -gt 0 ]; then
        log_success "Communication works after reconnect ($recv frames)"
        return 0
    else
        log_fail "Communication failed after reconnect"
        return 1
    fi
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: USB Disconnect Test"
    log_info "=========================================="
    echo ""

    if ! check_module_loaded; then
        log_error "bmcan module not loaded"
        return 1
    fi

    local original_ports
    local original_count
    original_ports=$(get_can_interfaces)
    original_count=$(count_can_interfaces)
    log_info "Current CAN interfaces ($original_count): $original_ports"

    if [ "$original_count" -eq 0 ]; then
        log_error "No CAN interfaces found — device not connected?"
        return 1
    fi

    echo ""
    log_warn "============================================================"
    log_warn "This test requires PHYSICAL USB DISCONNECT of a device."
    log_warn "Automated substitute: unbind/rebind USB device."
    log_warn "============================================================"
    echo ""

    # Record oops baseline before any USB operations
    local oops_baseline=$(record_oops_baseline)

    # --- Test 1: Simulate disconnect via unbind ---
    start_testcase "USB_Unbind_Ports_Disappear"

    # Find the USB device path for the second device (to keep one alive)
    local usb_devs=()
    for d in /sys/bus/usb/drivers/bmcan/*/; do
        [ -d "$d" ] && usb_devs+=("$d")
    done

    if [ ${#usb_devs[@]} -lt 1 ]; then
        log_warn "Cannot find USB device for unbind test — skipping"
        test_pass "USB_Unbind_Skipped_No_Device"
    else
        # Get the first USB device's interface
        local usb_intf=""
        for d in "${usb_devs[@]}"; do
            if [ -d "$d" ]; then
                usb_intf=$(basename "$d")
                break
            fi
        done

        if [ -n "$usb_intf" ] && [ -d "/sys/bus/usb/drivers/bmcan/$usb_intf" ]; then
            local count_before=$(count_can_interfaces)
            log_info "Unbinding USB interface: $usb_intf (ports before: $count_before)"

            # Count ports belonging to this device
            local ports_before=$(ls /sys/bus/usb/drivers/bmcan/$usb_intf/*/net/ 2>/dev/null | wc -l) || ports_before=0
            log_info "Ports on this device: $ports_before"

            # Unbind
            echo "$usb_intf" | sudo tee /sys/bus/usb/drivers/bmcan/unbind 2>/dev/null
            sleep 2

            local count_after=$(count_can_interfaces)
            log_info "Ports after unbind: $count_after"

            if test_ports_disappear_on_disconnect $count_before $count_after; then
                test_pass "Ports disappear on USB unbind"
            else
                test_fail "Ports did not disappear on USB unbind"
            fi

            # Check kernel health after unbind
            if check_kernel_health "$oops_baseline"; then
                log_success "Kernel stable after unbind"
            else
                test_fail "Kernel instability after unbind"
            fi

            # --- Test 2: Rebind to restore ---
            start_testcase "USB_Rebind_Ports_Restore"
            log_info "Rebinding USB interface: $usb_intf"

            echo "$usb_intf" | sudo tee /sys/bus/usb/drivers/bmcan/bind 2>/dev/null
            sleep 3

            local count_rebind=$(count_can_interfaces)
            log_info "Ports after rebind: $count_rebind"

            if test_ports_reappear_on_reconnect $count_before $count_rebind; then
                test_pass "Ports restored on USB rebind"
            else
                test_fail "Ports not fully restored on USB rebind"
            fi

            # Check kernel health after rebind
            if check_kernel_health "$oops_baseline"; then
                log_success "Kernel stable after rebind"
            else
                test_fail "Kernel instability after rebind"
            fi

            # --- Test 3: No resource leak ---
            start_testcase "USB_No_Resource_Leak"
            if test_no_resource_leak $count_before $count_rebind; then
                test_pass "No resource leak after unbind/rebind cycle"
            else
                test_fail "Resource leak detected"
            fi
        else
            log_warn "Cannot find valid USB interface — skipping unbind test"
            test_pass "USB_Unbind_Skipped"
        fi
    fi

    # --- Test 4: Communication after reconnect ---
    start_testcase "USB_Communication_After_Reconnect"
    # Use known cross-device port pair from hardware config
    if [ -f "$RESULTS_DIR/connected_ports.txt" ]; then
        local test_ports=($(cat "$RESULTS_DIR/connected_ports.txt"))
        local tx=${test_ports[0]}
        local rx=${test_ports[1]}
        log_info "Testing communication: $tx -> $rx (cross-device pair)"
        if test_communication_after_reconnect $tx $rx; then
            test_pass "Communication works after USB reconnect"
        else
            test_fail "Communication failed after USB reconnect"
        fi
    else
        local ports=($(get_can_interfaces))
        if [ ${#ports[@]} -ge 2 ]; then
            local tx=${ports[0]}
            local rx=${ports[1]}
            log_info "Testing communication: $tx -> $rx"
            if test_communication_after_reconnect $tx $rx; then
                test_pass "Communication works after USB reconnect"
            else
                test_fail "Communication failed after USB reconnect"
            fi
        else
            log_warn "Not enough ports for communication test — skipping"
            test_pass "USB_Comm_Skipped"
        fi
    fi

    echo ""
    log_success "USB disconnect test completed"
    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
