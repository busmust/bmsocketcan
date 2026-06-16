#!/bin/bash
################################################################################
# BMCAN Config Backup and Restore Library
#
# Manages CAN port configuration backup/restore for test isolation.
# Requires: API_BIN to be set (from lib/common.sh or run_all_tests.sh)
################################################################################

mkdir -p "$BACKUP_DIR"

################################################################################
# Config Backup
################################################################################

backup_port_config() {
    local port=$1
    local backup_file="$BACKUP_DIR/${port}_config.txt"

    sudo ip -details link show $port > "$backup_file"

    local term=$(sudo ip -details link show $port | grep "termination" | awk '{print $2}')
    local loopback=$(sudo ip -details link show $port | grep "loopback" | awk '{print $2}')
    local listen_only=$(sudo ip -details link show $port | grep "listen-only" | awk '{print $2}')

    cat > "$BACKUP_DIR/${port}_vars.sh" << EOF
TERMINATION="$term"
LOOPBACK="$loopback"
LISTEN_ONLY="$listen_only"
EOF

    echo "Config backed up: $port -> $backup_file"
    return 0
}

################################################################################
# Config Restore (Normal mode + CAN FD + 120 Ohm)
################################################################################

restore_port_config() {
    local port=$1

    sudo ip link set $port down >/dev/null 2>&1
    sleep 0.3

    sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 >/dev/null 2>&1
    sleep 0.5

    if ! ip link show $port | grep -q "UP"; then
        sudo ip link set $port up >/dev/null 2>&1
        sleep 0.3
    fi

    echo "Config restored: $port (Normal mode, CANFD 500K/2M, 120 Ohm)"
    return 0
}

restore_default_config() {
    local port=$1

    sudo ip link set $port down >/dev/null 2>&1
    sleep 0.3

    sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 >/dev/null 2>&1
    sleep 0.5

    if ! ip link show $port | grep -q "UP"; then
        sudo ip link set $port up >/dev/null 2>&1
        sleep 0.3
    fi

    echo "Default config restored: $port (Normal mode, CANFD 500K/2M, 120 Ohm)"
    return 0
}

################################################################################
# Config Verification
################################################################################

verify_port_config() {
    local port=$1

    if ! ip link show $port | grep -q "UP"; then
        echo "Port $port is not UP"
        return 1
    fi

    local details=$(ip -details link show $port)
    if echo "$details" | grep -q "LOOPBACK\|LISTEN-ONLY"; then
        echo "Port $port is in special mode (not normal mode)"
        return 1
    fi

    if ! echo "$details" | grep -q "<FD>"; then
        echo "Port $port CANFD not enabled"
        return 1
    fi

    local term=$(echo "$details" | grep "termination" | awk '{print $2}' || echo "0")
    if [ "$term" != "120" ]; then
        echo "Port $port termination is not 120 Ohm (current: $term)"
        return 1
    fi

    local berr_line=$(echo "$details" | grep "berr-counter" || echo "berr-counter tx - rx -")
    local tx_err=$(echo "$berr_line" | awk '{print $3}')
    local rx_err=$(echo "$berr_line" | awk '{print $5}')

    if ! [[ "$tx_err" =~ ^[0-9]+$ ]]; then tx_err="-"; fi
    if ! [[ "$rx_err" =~ ^[0-9]+$ ]]; then rx_err="-"; fi

    if [ "$tx_err" != "-" ] && [ "$rx_err" != "-" ]; then
        if [ "$tx_err" -gt 127 ] || [ "$rx_err" -gt 127 ]; then
            log_warn "Port $port has high error count (tx: $tx_err, rx: $rx_err)"
        fi
    fi

    echo "Port $port config verified: Normal mode, CANFD, 120 Ohm, errors(tx:$tx_err, rx:$rx_err)"
    return 0
}

################################################################################
# Config Clear Functions (uses bmcan_api)
################################################################################

clear_txtask() {
    local port=$1

    if [ -x "$API_BIN" ]; then
        sudo "$API_BIN" invalidate --txtask --tx-range 0-63 --device $port >/dev/null 2>&1
        echo "TX task cleared: $port"
    fi
}

clear_route() {
    local port=$1

    if [ -x "$API_BIN" ]; then
        sudo "$API_BIN" invalidate --route --route-range 0-3 --device $port >/dev/null 2>&1
        echo "Route cleared: $port"
    fi
}

clear_all_hw_config() {
    local port=$1

    if [ -x "$API_BIN" ]; then
        sudo "$API_BIN" invalidate --all --device $port >/dev/null 2>&1
        echo "All HW config cleared: $port"
    fi
}

################################################################################
# Cleanup
################################################################################

cleanup_backups() {
    rm -rf "$BACKUP_DIR"
    echo "Backup files cleaned"
}

# Export functions for use by sourced scripts
export -f backup_port_config
export -f restore_port_config
export -f restore_default_config
export -f verify_port_config
export -f clear_txtask
export -f clear_route
export -f clear_all_hw_config
export -f cleanup_backups
