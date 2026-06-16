#!/bin/bash
# Module: bm_replay.sh
# Test on-device replay (Gen2.5 only: E122, E142)

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

test_replay_config_on_off() {
    local port=$1

    log_info "Testing replay ON/OFF on $port..."

    # Configure replay with firmware defaults
    sudo $API_BIN replay --mode always --format blf --channels 0xFFFF \
        --cyclic 1 --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure replay on $port"
        return 1
    fi
    log_success "Replay started (BLF, ALWAYS, cyclic)"

    sleep 2

    sudo $API_BIN replay --mode off --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to stop replay"
        return 1
    fi
    log_success "Replay stopped"
    return 0
}

test_replay_bbd_format() {
    local port=$1

    log_info "Testing replay BBD format on $port..."

    sudo $API_BIN replay --mode always --format bbd --channels 0xFFFF \
        --cyclic 0 --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure BBD replay"
        return 1
    fi

    sleep 2

    sudo $API_BIN replay --mode off --device $port --apply >/dev/null 2>&1
    log_success "Replay BBD format test passed"
    return 0
}

test_replay_cyclic() {
    local port=$1

    log_info "Testing replay cyclic on $port..."

    # Cyclic ON
    sudo $API_BIN replay --mode always --format blf --channels 0xFFFF \
        --cyclic 1 --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure cyclic replay"
        return 1
    fi
    sleep 2

    # Cyclic OFF
    sudo $API_BIN replay --mode always --format blf --channels 0xFFFF \
        --cyclic 0 --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure non-cyclic replay"
        return 1
    fi
    sleep 2

    sudo $API_BIN replay --mode off --device $port --apply >/dev/null 2>&1
    log_success "Replay cyclic test passed"
    return 0
}

test_replay_channel_mask() {
    local port=$1

    log_info "Testing replay channel mask on $port..."

    # Single channel
    sudo $API_BIN replay --mode always --format blf --channels 0x0001 \
        --cyclic 0 --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure single-channel replay"
        return 1
    fi
    sleep 1

    # All channels
    sudo $API_BIN replay --mode always --format blf --channels 0xFFFF \
        --cyclic 0 --device $port --apply >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        log_fail "Failed to configure all-channel replay"
        return 1
    fi
    sleep 1

    sudo $API_BIN replay --mode off --device $port --apply >/dev/null 2>&1
    log_success "Replay channel mask test passed"
    return 0
}

################################################################################
# Main
################################################################################

main() {
    if ! is_gen25_device; then
        log_warn "No Gen2.5 device detected - skipping replay tests"
        return 0
    fi

    log_info "Gen2.5 device detected - running replay tests"
    append_testsuite

    local ports=$(cat "$CONNECTED_PORTS_FILE" 2>/dev/null)
    local port=""
    local port_list=($ports)
    port=${port_list[0]}

    if [ -z "$port" ]; then
        log_error "No test port available"
        return 0
    fi

    if [ ! -x "$API_BIN" ]; then
        log_error "API tool not found: $API_BIN"
        return 1
    fi

    start_testcase "Replay_Config_On_Off"
    if test_replay_config_on_off $port; then
        test_pass "Replay ON/OFF OK"
    else
        test_fail "Replay ON/OFF failed"
    fi

    start_testcase "Replay_BBD_Format"
    if test_replay_bbd_format $port; then
        test_pass "Replay BBD format OK"
    else
        test_fail "Replay BBD format failed"
    fi

    start_testcase "Replay_Cyclic"
    if test_replay_cyclic $port; then
        test_pass "Replay cyclic OK"
    else
        test_fail "Replay cyclic failed"
    fi

    start_testcase "Replay_Channel_Mask"
    if test_replay_channel_mask $port; then
        test_pass "Replay channel mask OK"
    else
        test_fail "Replay channel mask failed"
    fi

    return 0
}
