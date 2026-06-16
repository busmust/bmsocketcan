#!/bin/bash
################################################################################
# BMCAN Automated Test Suite - Main Entry Point
#
# Usage: bash run_all_tests.sh
#
# Phases:
#   Phase 0: Port initialization (uses known port pairs from hardware config)
#   Phase 1: Basic config tests (work mode)
#   Phase 2: Communication tests (basic, comprehensive, route, txtask, filter)
#   Phase 3: Advanced tests (config persistence, stress, generation-specific)
#     - Gen3 only: extreme FPS test
#     - Gen2.5 only: logging, replay tests
#   Output: JUnit XML report for Bamboo CI
################################################################################

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Import framework libraries
source "$SCRIPT_DIR/lib/common.sh"
source "$SCRIPT_DIR/lib/junit_xml.sh"
source "$SCRIPT_DIR/lib/config_manager.sh"

# Create results directory
mkdir -p "$RESULTS_DIR"

################################################################################
# Port Initialization
################################################################################

initialize_ports() {
    log_header "Phase 0: Port Initialization"

    # Load hardware config for port pairs
    if [ -f "$HW_CONF_BASH" ]; then
        source "$HW_CONF_BASH"
    else
        log_error "Hardware config not found: $HW_CONF_BASH"
        return 1
    fi

    # Use primary test pair from hardware config
    local test_ports="$HW_TEST_PORT_PRIMARY $HW_TEST_PORT_SECONDARY"
    log_info "Using port pair: $test_ports"

    # Initialize ports
    for port in $test_ports; do
        sudo ip link set $port down 2>/dev/null || true
    done
    sleep 0.3

    for port in $test_ports; do
        sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 2>/dev/null
        sleep 0.2
    done
    sleep 0.5

    # Verify port status
    local all_ok=true
    for port in $test_ports; do
        if ! ip link show $port | grep -q "UP"; then
            log_error "Port $port failed to initialize"
            all_ok=false
        fi
    done

    if [ "$all_ok" = false ]; then
        return 1
    fi

    # Save port list
    echo "$test_ports" | tr ' ' '\n' > "$CONNECTED_PORTS_FILE"
    log_success "Ports initialized: $(cat $CONNECTED_PORTS_FILE | tr '\n' ' ')"

    return 0
}

################################################################################
# Test Phases
################################################################################

run_phase1_config_tests() {
    log_header "Phase 1: Configuration Tests"
    local phase_fail=0

    local modules=(
        "bm_mode_test.sh"
    )

    for module in "${modules[@]}"; do
        local module_path="$MODULES_DIR/$module"
        if [ -f "$module_path" ]; then
            log_info "Running $module..."
            source "$module_path"
            if main "$@"; then
                :
            else
                log_error "Module $module FAILED"
                phase_fail=$((phase_fail + 1))
            fi
        else
            log_error "Module not found: $module_path"
            phase_fail=$((phase_fail + 1))
        fi
    done

    return $phase_fail
}

run_phase2_comm_tests() {
    log_header "Phase 2: Communication Tests"
    local phase_fail=0

    local modules=(
        "bm_basic_communication.sh"
        "bm_comprehensive_communication.sh"
        "bm_data_integrity.sh"
        "bm_termination.sh"
        "bm_doc_commands.sh"
        "bm_route.sh"
        "bm_tx_task.sh"
        "bm_filter.sh"
        "bm_multi_pair_test.sh"
    )

    for module in "${modules[@]}"; do
        local module_path="$MODULES_DIR/$module"
        if [ -f "$module_path" ]; then
            log_info "Running $module..."
            source "$module_path"
            if main "$@"; then
                :
            else
                log_error "Module $module FAILED"
                phase_fail=$((phase_fail + 1))
            fi
        else
            log_error "Module not found: $module_path"
            phase_fail=$((phase_fail + 1))
        fi
    done

    return $phase_fail
}

run_phase3_advanced_tests() {
    log_header "Phase 3: Advanced Features"
    local phase_fail=0

    local gen=$(detect_generation)
    log_info "Detected device generation: $gen"

    # Modules that always run
    local modules=(
        "bm_config_persistence.sh"
        "bm_stress_test.sh"
        "bm_boundary_test.sh"
        "bm_timestamp.sh"
        "bm_busoff_recovery.sh"
        "bm_busoff_stress.sh"
        "bm_usb_disconnect.sh"
    )

    # Gen2.5 only: logging and replay
    if [ "$gen" = "gen25" ]; then
        modules+=("bm_logging.sh" "bm_replay.sh")
        log_info "Gen2.5: including logging and replay tests"
    else
        log_info "Skipping logging/replay tests (Gen2.5 only)"
    fi

    # Gen3 only: extreme FPS
    if [ "$gen" = "gen3" ]; then
        modules+=("bm_extreme_fps_test.sh")
        log_info "Gen3: including extreme FPS test"
    else
        log_info "Skipping extreme FPS test (Gen3 only)"
    fi

    for module in "${modules[@]}"; do
        local module_path="$MODULES_DIR/$module"
        if [ -f "$module_path" ]; then
            log_info "Running $module..."
            source "$module_path"
            if main "$@"; then
                :
            else
                log_error "Module $module FAILED"
                phase_fail=$((phase_fail + 1))
            fi
        else
            log_error "Module not found: $module_path"
            phase_fail=$((phase_fail + 1))
        fi
    done

    return $phase_fail
}

################################################################################
# Helper Functions
################################################################################

reset_ports_normal() {
    local ports=$(cat "$CONNECTED_PORTS_FILE")
    for port in $ports; do
        sudo ip link set $port down 2>/dev/null || true
    done
    sleep 0.3
    for port in $ports; do
        sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 2>/dev/null || true
    done
    sleep 0.5
    log_success "Ports reset to normal mode"
}

reset_ports_cleanup() {
    local ports=$(cat "$CONNECTED_PORTS_FILE")
    for port in $ports; do
        sudo "$API_BIN" clear --device $port >/dev/null 2>&1 || true
        sudo "$API_BIN" invalidate --txtask --tx-range 0-63 --device $port >/dev/null 2>&1 || true
        sudo "$API_BIN" invalidate --route --route-range 0-63 --device $port >/dev/null 2>&1 || true
    done
    sleep 0.3
    reset_ports_normal
}

################################################################################
# Main
################################################################################

main() {
    local start_time=$(date)
    log_header "BMCAN Automated Test Suite"
    log_info "Start time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""

    # Cleanup residual state
    log_info "Cleaning up residual state..."
    killall -9 candump cansend cangen 2>/dev/null || true
    sleep 0.5
    # Down all CAN ports before rmmod to release references
    for p in $(ls /sys/class/net/ 2>/dev/null | grep can); do
        sudo ip link set $p down 2>/dev/null || true
    done
    sleep 0.3

    # Reload driver for clean state
    log_info "Reloading driver for clean state..."
    if lsmod | grep -q bmcan; then
        sudo rmmod bmcan 2>/dev/null || true
        sleep 0.5
    fi
    sudo insmod "$DRIVER_PATH" 2>/dev/null
    sleep 1

    # Init test suite
    init_testsuite "BMCAN Tests"

    # Phase 0: Port initialization
    if ! initialize_ports; then
        log_error "Port initialization failed - aborting all tests"
        return 1
    fi

    echo ""

    # Phase 1: Config tests
    local total_fail=0
    run_phase1_config_tests || total_fail=$((total_fail + $?))

    # Full port reinit after Phase 1 mode changes with longer settle time
    log_info "Reinitializing ports after Phase 1 mode test..."
    local ports=($(cat "$CONNECTED_PORTS_FILE"))
    for port in "${ports[@]}"; do
        sudo ip link set $port down 2>/dev/null || true
    done
    sleep 2
    for port in "${ports[@]}"; do
        sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 2>/dev/null || true
        sleep 0.5
    done
    sleep 2
    log_success "Ports reinitialized with extended settle time"
    echo ""

    # Connectivity warm-up with retries
    local tx=${ports[0]}
    local rx=${ports[1]}
    local warmup_ok=false

    for attempt in 1 2 3 4 5; do
        log_info "Connectivity check attempt $attempt: $tx -> $rx..."
        timeout 3 candump $rx -n 1 -t A 2>/dev/null > /tmp/warmup.log &
        local warmup_pid=$!
        sleep 0.3
        sudo cansend $tx 0FF#AABBCCDD >/dev/null 2>&1
        wait $warmup_pid 2>/dev/null || true
        local warmup_recv; warmup_recv=$(grep -c "0FF" /tmp/warmup.log 2>/dev/null) || warmup_recv=0
        rm -f /tmp/warmup.log

        if [ "$warmup_recv" -gt 0 ]; then
            log_success "Connectivity verified: $warmup_recv frames received"
            warmup_ok=true
            break
        else
            log_warn "Attempt $attempt: no frames received, retrying..."
            sleep 1
        fi
    done

    if [ "$warmup_ok" = false ]; then
        log_error "Connectivity check failed after 5 attempts - communication tests may fail"
    fi
    echo ""

    # Phase 2: Communication tests
    run_phase2_comm_tests || total_fail=$((total_fail + $?))

    log_info "Resetting ports after Phase 2..."
    reset_ports_cleanup
    echo ""

    # Phase 3: Advanced tests
    run_phase3_advanced_tests || total_fail=$((total_fail + $?))
    echo ""

    # Generate report
    log_header "Generating Test Reports"

    generate_junit_xml "$JUNIT_REPORT"
    log_success "JUnit XML report: $JUNIT_REPORT"

    print_test_summary

    local end_time=$(date)
    log_info "End time: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""

    log_header "Test Complete"
    log_info "Report: $JUNIT_REPORT"

    if [ "$total_fail" -gt 0 ]; then
        log_error "$total_fail module(s) failed or missing — test suite incomplete"
        return 1
    fi

    # Check JUnit-level failures (test_fail inside modules)
    if [ "${BMCAN_TOTAL_FAILURES:-0}" -gt 0 ]; then
        log_error "$BMCAN_TOTAL_FAILURES test case(s) failed (see JUnit report)"
        return 1
    fi

    return 0
}

main "$@"
