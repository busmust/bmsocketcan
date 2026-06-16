#!/bin/bash
# Module: bm_logging.sh
# Test on-device logging (Gen2.5 only: E122, E142)

# Import libraries
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# Gen2.5 Detection
################################################################################

is_gen25_device() {
    [ "$(detect_generation)" = "gen25" ]
}

################################################################################
# Test Functions
################################################################################

test_logging_record() {
    local port=$1

    log_info "Testing logging record (BLF, ALWAYS_ON) on $port..."

    # Configure logging with firmware defaults (no path params)
    sudo $API_BIN logging --mode always --format blf --channels 0xFFFF \
        --direction all --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure logging on $port"
        return 1
    fi
    log_success "Logging started (BLF, ALWAYS_ON, default path)"

    # Send test frames
    local tx_count=20
    cangen $port -g 10 -L 8 -n $tx_count >/dev/null 2>&1 || true

    # Wait for flush
    sleep 3

    # Stop logging
    sudo $API_BIN logging --mode off --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to stop logging"
        return 1
    fi
    log_success "Logging stopped ($tx_count frames recorded)"
    return 0
}

test_logging_off() {
    local port=$1

    log_info "Testing logging OFF on $port..."

    sudo $API_BIN logging --mode off --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to set logging OFF"
        return 1
    fi
    log_success "Logging OFF"
    return 0
}

test_logging_bbd_format() {
    local port=$1

    log_info "Testing logging BBD format on $port..."

    sudo $API_BIN logging --mode always --format bbd --channels 0xFFFF \
        --direction all --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure BBD logging"
        return 1
    fi

    cangen $port -g 50 -L 8 -n 5 >/dev/null 2>&1 || true
    sleep 2

    sudo $API_BIN logging --mode off --device $port --apply >/dev/null 2>&1
    log_success "Logging BBD format test passed"
    return 0
}

test_logging_direction() {
    local port=$1

    log_info "Testing logging direction filter on $port..."

    # RX only
    sudo $API_BIN logging --mode always --format blf --direction rx \
        --channels 0xFFFF --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to set RX-only logging"
        return 1
    fi
    sleep 1

    # TX only
    sudo $API_BIN logging --mode always --format blf --direction tx \
        --channels 0xFFFF --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to set TX-only logging"
        return 1
    fi
    sleep 1

    # All
    sudo $API_BIN logging --mode always --format blf --direction all \
        --channels 0xFFFF --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to set ALL-direction logging"
        return 1
    fi

    sudo $API_BIN logging --mode off --device $port --apply >/dev/null 2>&1
    log_success "Logging direction filter test passed"
    return 0
}

################################################################################
# Main
################################################################################

main() {
    if ! is_gen25_device; then
        log_warn "No Gen2.5 device detected - skipping logging tests"
        return 0
    fi

    log_info "Gen2.5 device detected - running logging tests"
    append_testsuite

    local ports=$(cat "$CONNECTED_PORTS_FILE" 2>/dev/null)
    local port=""
    for p in $ports; do
        port=$p
        break
    done

    if [ -z "$port" ]; then
        log_error "No test port available"
        return 0
    fi

    if [ ! -x "$API_BIN" ]; then
        log_error "API tool not found: $API_BIN"
        return 1
    fi

    start_testcase "Logging_Record"
    if test_logging_record $port; then
        test_pass "Logging record OK"
    else
        test_fail "Logging record failed"
    fi

    start_testcase "Logging_Off"
    if test_logging_off $port; then
        test_pass "Logging OFF OK"
    else
        test_fail "Logging OFF failed"
    fi

    start_testcase "Logging_BBD_Format"
    if test_logging_bbd_format $port; then
        test_pass "Logging BBD format OK"
    else
        test_fail "Logging BBD format failed"
    fi

    start_testcase "Logging_Direction"
    if test_logging_direction $port; then
        test_pass "Logging direction filter OK"
    else
        test_fail "Logging direction filter failed"
    fi

    return 0
}
