#!/bin/bash
# Module 8: Config Persistence Test
# Function: save/load configuration test


# Import libraries
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

# Detect device generation (uses common.sh detect_generation from lsusb)
has_storage() {
    local gen=$(detect_generation)
    [ "$gen" = "gen25" ] || [ "$gen" = "gen3" ]
}

################################################################################
# Test Functions
################################################################################

test_save_config() {
    local port=$1
    local mask=${2:-0xFFFF}
    log_info "Saving configuration to hardware (mask=0x$(printf '%04X' $mask))"

    if sudo $API_BIN save --device $port --mask $(printf '%04X' $mask) >/dev/null 2>&1; then
        log_success "Config saved to hardware"
        return 0
    else
        log_fail "Save config failed"
        return 1
    fi
}

test_load_config() {
    local port=$1
    local mask=${2:-0xFFFF}
    log_info "Loading configuration from hardware (mask=0x$(printf '%04X' $mask))"

    if sudo $API_BIN load --device $port --mask $(printf '%04X' $mask) >/dev/null 2>&1; then
        log_success "Config loaded from hardware"
        return 0
    else
        log_fail "Load config failed"
        return 1
    fi
}

test_verify_persistence() {
    local port=$1
    local rx=$2
    log_info "Verifying config persistence"

    # Save txtask only (mask=0x0200), avoids mode/bitrate switching
    test_save_config $port 0x0200 || return 1

    # Modify to different ID (no --apply)
    sudo $API_BIN txtasks --type fixed --id 0x200 --payload CC000001 \
        --cycle 100 --index 0 --device $port >/dev/null 2>&1

    sleep 0.5

    # Load txtask only (mask=0x0200)
    test_load_config $port 0x0200 || return 1

    sleep 1

    # Verify: Check if ID=0x100 is restored
    local frames=$(timeout 3 candump $rx 2>/dev/null | grep -c "100" || echo 0)
    frames=${frames:-0}
    [[ "$frames" =~ ^[0-9]+$ ]] || frames=0

    if [ "$frames" -gt 0 ]; then
        log_success "Config persistence verified (ID=0x100 restored)"
        return 0
    else
        log_fail "Config not restored correctly"
        return 1
    fi
}

test_clear_config() {
    local port=$1
    log_info "Clearing hardware configuration"

    local result=0
    # Clear saved config from device storage (prevent offline config from taking effect)
    sudo $API_BIN clear --device $port >/dev/null 2>&1 || result=1
    # Also invalidate runtime txtask/route tables
    sudo $API_BIN invalidate --txtask --tx-range 0-63 --device $port >/dev/null 2>&1 || true
    sudo $API_BIN invalidate --route --route-range 0-63 --device $port >/dev/null 2>&1 || true

    if [ $result -eq 0 ]; then
        log_success "Config cleared (device storage + runtime)"
        return 0
    else
        log_fail "Clear config failed"
        return 1
    fi
}

################################################################################
# Main Test Flow
################################################################################

main() {
    log_info "=========================================="
    log_info "Module 8: Config Persistence Test"
    log_info "=========================================="
    echo ""


    # Check API tool
    if [ ! -x "$API_BIN" ]; then
        log_error "API tool not found: $API_BIN"
        return 1
    fi

    # Read ports
    if [ ! -f "$SCRIPT_DIR/../results/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi

    local ports=($(cat "$SCRIPT_DIR/../results/connected_ports.txt"))
    local port=${ports[0]}
    local rx_port=${ports[1]}

    log_info "Test port: $port"
    log_info "RX port: $rx_port"
    echo ""

    # Detect device generation
    if has_storage; then
        log_warn "Storage-capable device detected - running persistence test"
    else
        log_warn "No storage-capable device detected - skipping persistence test"
        return 0
    fi
    echo ""

    # Configure txtask for testing
    log_info "Setting up initial txtask (ID=0x100)..."
    log_info "Device operstate: $(cat /sys/class/net/$port/operstate 2>&1)"
    local setup_output
    setup_output=$(sudo $API_BIN txtasks --type fixed --id 0x100 --payload AABBCCDD \
        --cycle 100 --index 0 --device $port --apply 2>&1)
    local setup_rc=$?
    log_info "Initial setup rc=$setup_rc output=${setup_output:0:200}"
    if [ $setup_rc -ne 0 ]; then
        log_error "Initial txtask setup FAILED (rc=$setup_rc)"
        log_error "Retrying with delay..."
        sleep 1
        setup_output=$(sudo $API_BIN txtasks --type fixed --id 0x100 --payload AABBCCDD \
            --cycle 100 --index 0 --device $port --apply 2>&1)
        setup_rc=$?
        log_info "Retry rc=$setup_rc output=${setup_output:0:200}"
    fi
    sleep 0.5
    echo ""

    # Test 1: Save configuration (txtask only)
    start_testcase "Config_Save_To_Hardware"
    if test_save_config $port 0x0200; then
        test_pass "Save config successful"
    else
        test_fail "Save config failed (may not be supported)"
    fi
    echo ""

    # Test 2: Load configuration (txtask only)
    start_testcase "Config_Load_From_Hardware"
    if test_load_config $port 0x0200; then
        test_pass "Load config successful"
    else
        test_fail "Load config failed"
    fi
    echo ""

    # Test 3: Verify persistence
    start_testcase "Config_Verify_Restore"
    if test_verify_persistence $port $rx_port; then
        test_pass "Config persistence verified"
    else
        test_fail "Config persistence verification failed"
    fi
    echo ""

    # Cleanup: Clear all configurations
    log_info "Cleaning up..."
    start_testcase "Config_Clear"
    if test_clear_config $port; then
        test_pass "Config cleared"
    else
        test_fail "Config clear failed"
    fi
    echo ""

    # Skip configuration restore (will cause test case loss)
    log_info "Skipping configuration restore to preserve test results..."
    # restore_port_config $port
    # verify_port_config $port
    echo ""

    log_success "Config persistence test completed"
    return 0
}

