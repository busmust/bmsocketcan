#!/bin/bash
################################################################################
# BMCAN Hardware Helper Library
#
# Provides functions to query and use hardware configuration.
# Requires: lib/common.sh to be sourced first (for colors and logging).
#
# Usage:
#   source "$SCRIPT_DIR/lib/common.sh"
#   source "$SCRIPT_DIR/lib/hardware_helper.sh"
#   hw_init
#   hw_get_primary_pair TX RX
################################################################################

################################################################################
# Initialization
################################################################################

hw_init() {
    if [ -f "$HW_CONF_BASH" ]; then
        source "$HW_CONF_BASH"
        return 0
    else
        log_error "Hardware config not found: $HW_CONF_BASH"
        return 1
    fi
}

################################################################################
# Query Functions
################################################################################

# Get primary test ports
# Usage: hw_get_primary_pair TX_VAR RX_VAR
hw_get_primary_pair() {
    local tx_var=$1
    local rx_var=$2
    eval "$tx_var='$HW_TEST_PORT_PRIMARY'"
    eval "$rx_var='$HW_TEST_PORT_SECONDARY'"
}

# Get connection partner for a port
# Usage: partner=$(hw_get_partner "can0")
hw_get_partner() {
    local port=$1
    local var_name="HW_${port^^}_CONNECTED_TO"
    echo "${!var_name}"
}

# Get all connection pairs as space-separated string
hw_get_pairs_array() {
    hw_init
    local pairs=()
    for pair in $HW_CONN_PAIRS; do
        pairs+=("$pair")
    done
    echo "${pairs[@]}"
}

# Check if mode is supported
hw_is_mode_supported() {
    local mode=$1
    if [[ "$HW_UNSUPPORTED_MODES" == *"$mode"* ]]; then
        return 1
    else
        return 0
    fi
}

################################################################################
# Display Functions
################################################################################

hw_show_matrix() {
    hw_init
    echo ""
    echo "=========================================="
    echo -e "${BLUE}Hardware Connection Matrix${NC}"
    echo "=========================================="
    echo ""
    echo "           Device 1 (USB 1-1)"
    echo "           +---------------+"
    echo "    can0   |               |   can6"
    echo "    -------+               +-------   OK"
    echo ""
    echo "    can1   |               |   can7"
    echo "    -------+               +-------   OK"
    echo ""
    echo "    can2   |   Device 2    |   can4"
    echo "    -------+  (USB 1-2)    +-------   OK"
    echo ""
    echo "    can3   |               |   can5"
    echo "    -------+               +-------   OK"
    echo "           +---------------+"
    echo ""
}

hw_show_channels() {
    hw_init
    echo ""
    echo "=========================================="
    echo -e "${BLUE}Available CAN Channels${NC}"
    echo "=========================================="
    printf "%-8s %-15s %-15s %-10s\n" "Channel" "Device" "Connected To" "Status"
    printf "%-8s %-15s %-15s %-10s\n" "--------" "---------------" "---------------" "----------"

    for i in {0..7}; do
        local port="can$i"
        local dev_var="HW_CAN${i}_DEV"
        local conn_var="HW_CAN${i}_CONNECTED_TO"
        local avail_var="HW_CAN${i}_AVAILABLE"

        printf "%-8s %-15s %-15s " "$port" "${!dev_var}" "${!conn_var}"

        if [ "${!avail_var}" = "true" ]; then
            echo -e "${GREEN}Available${NC}"
        else
            echo -e "${RED}N/A${NC}"
        fi
    done
    echo ""
}

hw_show_summary() {
    hw_init
    echo ""
    echo "=========================================="
    echo -e "${BLUE}Hardware Configuration Summary${NC}"
    echo "=========================================="
    echo ""
    echo "Total Devices:     $HW_DEVICE_COUNT"
    echo "Total Channels:    $HW_TOTAL_CHANNELS"
    echo "Connection Pairs:  4 (all bidirectional)"
    echo ""
    echo "Primary Test Pair: $HW_TEST_PORT_PRIMARY <-> $HW_TEST_PORT_SECONDARY"
    echo ""
    echo "Supported Modes:   $HW_SUPPORTED_MODES"
    echo "Unsupported:       $HW_UNSUPPORTED_MODES"
    echo ""
}

################################################################################
# Test Helper Functions
################################################################################

# Initialize a pair of ports for testing with CAN FD enabled
# Usage: hw_init_pair "can0" "can6" [bitrate] [termination] [dbitrate]
hw_init_pair() {
    local tx=$1
    local rx=$2
    local bitrate=${3:-500000}
    local termination=${4:-120}
    local dbitrate=${5:-2000000}

    echo -e "${YELLOW}Initializing $tx and $rx (CANFD mode)...${NC}"

    sudo ip link set $tx down 2>/dev/null || true
    sudo ip link set $rx down 2>/dev/null || true
    sleep 0.2

    sudo ip link set $tx up type can bitrate $bitrate sample-point 0.875 dbitrate $dbitrate dsample-point 0.750 fd on termination $termination 2>/dev/null
    sudo ip link set $rx up type can bitrate $bitrate sample-point 0.875 dbitrate $dbitrate dsample-point 0.750 fd on termination $termination 2>/dev/null
    sleep 0.3

    local tx_info=$(ip link show $tx 2>/dev/null)
    local rx_info=$(ip link show $rx 2>/dev/null)

    if echo "$tx_info" | grep -q "UP" && echo "$rx_info" | grep -q "UP"; then
        log_success "$tx and $rx initialized (CANFD: $bitrate/$dbitrate)"
        return 0
    else
        log_fail "Failed to initialize ports"
        echo "  TX state: $tx_info"
        echo "  RX state: $rx_info"
        return 1
    fi
}

# Reset all ports to default state (CAN FD enabled)
hw_reset_all_ports() {
    echo -e "${YELLOW}Resetting all CAN ports (CANFD mode)...${NC}"
    for port in $HW_ALL_CHANNELS; do
        sudo ip link set $port down 2>/dev/null || true
        sleep 0.1
        sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 2>/dev/null || true
        sleep 0.1
    done
    sleep 0.5
    log_success "All ports reset to CANFD mode"
}

# Quick connectivity test between two ports
# Usage: hw_test_connection "can0" "can6"
hw_test_connection() {
    local tx=$1
    local rx=$2

    timeout 1 candump $rx > /tmp/hw_test.log 2>/dev/null &
    local pid=$!
    sleep 0.2

    sudo cansend $tx 123#DEADBEEF >/dev/null 2>&1
    sleep 0.4

    pkill -P $pid 2>/dev/null || true
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true

    if [ -s /tmp/hw_test.log ]; then
        rm -f /tmp/hw_test.log
        return 0
    else
        rm -f /tmp/hw_test.log
        return 1
    fi
}

################################################################################
# Main
################################################################################

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    source "$SCRIPT_DIR/common.sh"
    echo "BMCAN Hardware Helper Library"
    echo ""
    echo "Usage: source ./lib/hardware_helper.sh"
    echo ""
    echo "Available functions:"
    echo "  hw_init                    - Load hardware config"
    echo "  hw_get_primary_pair TX RX  - Get primary test ports"
    echo "  hw_get_partner PORT        - Get connection partner"
    echo "  hw_show_matrix             - Display connection matrix"
    echo "  hw_show_channels           - List all channels"
    echo "  hw_show_summary            - Show configuration summary"
    echo "  hw_init_pair TX RX         - Initialize port pair"
    echo "  hw_reset_all_ports         - Reset all to defaults"
    echo "  hw_test_connection TX RX   - Test connectivity"
    echo ""
fi
