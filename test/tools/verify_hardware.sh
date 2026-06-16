#!/bin/bash
################################################################################
# BMCAN Hardware Verification Tool
#
# Verifies hardware configuration and connectivity for all port pairs.
#
# Usage: bash tools/verify_hardware.sh
################################################################################

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Go up to test/ root for common.sh
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

source "$PROJECT_DIR/lib/common.sh"
source "$PROJECT_DIR/lib/hardware_helper.sh"
source "$PROJECT_DIR/lib/junit_xml.sh"

RESULTS_DIR="$PROJECT_DIR/results"

################################################################################
# Main
################################################################################

main() {
    mkdir -p "$RESULTS_DIR"

    echo ""
    echo "=========================================="
    echo -e "${BLUE}BMCAN Hardware Verification Tool${NC}"
    echo "=========================================="
    echo ""

    hw_init || exit 1

    hw_show_summary
    hw_show_matrix
    hw_show_channels

    # Verify each connection pair
    echo "=========================================="
    echo -e "${BLUE}Verifying Connection Pairs${NC}"
    echo "=========================================="
    echo ""

    init_testsuite "Hardware Verification"

    for pair in $HW_CONN_PAIRS; do
        local tx=${pair%:*}
        local rx=${pair#*:}

        start_testcase "Verify_$tx"_"$rx"

        echo -n "Testing $tx -> $rx: "

        if ! hw_init_pair "$tx" "$rx"; then
            end_testcase "fail" "Failed to initialize ports"
            log_fail "Init failed"
            continue
        fi

        if hw_test_connection "$tx" "$rx"; then
            end_testcase "pass" "Connection verified"
            log_success "Connected"
        else
            end_testcase "fail" "No connectivity"
            log_fail "Not connected"
        fi

        echo -n "Testing $rx -> $tx: "
        if hw_test_connection "$rx" "$tx"; then
            log_success "Connected"
        else
            log_fail "Not connected"
        fi

        echo ""
    done

    generate_junit_xml "$RESULTS_DIR/hardware_verification.xml"
    log_success "Report saved: $RESULTS_DIR/hardware_verification.xml"
    echo ""

    hw_reset_all_ports

    echo "=========================================="
    echo -e "${BLUE}Verification Complete${NC}"
    echo "=========================================="
}

main "$@"
