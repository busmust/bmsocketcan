#!/bin/bash
################################################################################
# BMCAN Hardware Configuration Update Tool
#
# Usage:
#   bash tools/update_hardware_config.sh          # Interactive mode
#   bash tools/update_hardware_config.sh --auto   # Auto-discovery mode
#   bash tools/update_hardware_config.sh --set can0:can6 can1:can7
################################################################################

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

source "$PROJECT_DIR/lib/common.sh"
source "$PROJECT_DIR/lib/hardware_helper.sh" 2>/dev/null || true

################################################################################
# Functions
################################################################################

show_banner() {
    echo ""
    echo "=========================================="
    echo -e "${CYAN}BMCAN Hardware Configuration Update${NC}"
    echo "=========================================="
    echo ""
}

# Scan available CAN devices
scan_devices() {
    echo -e "${BLUE}Scanning for CAN devices...${NC}"
    echo ""

    local can_ports=$(ls /sys/class/net/ | grep "^can[0-9]$" | sort -V | tr '\n' ' ')

    if [ -z "$can_ports" ]; then
        log_error "No CAN devices found!"
        echo "Please ensure:"
        echo "  1. BMCAN driver is loaded"
        echo "  2. USB devices are connected"
        echo "  3. Run 'sudo modprobe bmcan' if needed"
        return 1
    fi

    # Group by USB device
    declare -A device_ports
    declare -A device_names

    for port in $can_ports; do
        local dev_path="/sys/class/net/$port/device"
        if [ -e "$dev_path" ]; then
            local dev_id=$(readlink -f "$dev_path" | xargs basename)
            device_ports[$dev_id]="${device_ports[$dev_id]} $port"

            local usb_bus=$(echo "$dev_id" | cut -d: -f1)
            local usb_device_info=$(lsusb | grep "Bus $usb_bus" | head -1)
            if [ -n "$usb_device_info" ]; then
                device_names[$dev_id]="$usb_device_info"
            fi
        fi
    done

    local dev_count=0
    for dev_id in $(echo "${!device_ports[@]}" | sort); do
        ((dev_count++))
        echo -e "${GREEN}Device $dev_count: $dev_id${NC}"
        echo "  Ports:${device_ports[$dev_id]}"
        if [ -n "${device_names[$dev_id]}" ]; then
            echo "  Info: ${device_names[$dev_id]}"
        fi
        echo ""
    done

    HW_TOTAL_CHANNELS=$(echo "$can_ports" | wc -w)
    HW_DEVICE_COUNT=$dev_count
    HW_ALL_CHANNELS="$can_ports"

    log_success "Found: $HW_DEVICE_COUNT device(s), $HW_TOTAL_CHANNELS channel(s)"
    echo "Channels: $HW_ALL_CHANNELS"
    echo ""

    return 0
}

# Auto-discover connections by testing all port combinations
auto_discover_connections() {
    echo -e "${BLUE}Auto-discovering physical connections...${NC}"
    echo ""
    echo "This will test all possible port combinations."
    echo "Please ensure all CAN ports are initialized."
    echo ""

    local ports=$(echo "$HW_ALL_CHANNELS" | tr ' ' '\n' | sort -V)

    echo -e "${YELLOW}Initializing all ports...${NC}"
    for port in $ports; do
        sudo ip link set $port down 2>/dev/null || true
        sleep 0.1
        sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120 2>/dev/null || true
        sleep 0.1
    done
    sleep 0.5
    log_success "All ports initialized"
    echo ""

    declare -A connections
    local tested=0
    local total_tests=0

    for tx in $ports; do
        for rx in $ports; do
            if [ "$tx" != "$rx" ]; then
                ((total_tests++))
                if [ -n "${connections[$rx]}" ] && [[ "${connections[$rx]}" == *"$tx"* ]]; then
                    continue
                fi

                ((tested++))
                local progress=$((tested * 100 / total_tests))
                echo -ne "Testing... ($progress%)\r"

                timeout 1 candump $rx > /tmp/discover_test.log 2>/dev/null &
                local pid=$!
                sleep 0.15
                sudo cansend $tx 100#DEADBEEF >/dev/null 2>&1
                sleep 0.3
                pkill -P $pid 2>/dev/null || true
                kill $pid 2>/dev/null || true
                wait $pid 2>/dev/null || true

                if [ -s /tmp/discover_test.log ]; then
                    connections[$tx]="$rx"
                    log_success "Found: $tx -> $rx"
                fi
                rm -f /tmp/discover_test.log
            fi
        done
    done

    echo ""
    log_success "Discovery complete!"
    echo ""

    local pairs_list=""
    for tx in $(echo "${!connections[@]}" | sort -V); do
        local rx="${connections[$tx]}"
        pairs_list="$pairs_list $tx:$rx"
    done

    HW_CONN_PAIRS=$(echo "$pairs_list" | tr ' ' '\n' | grep -v '^$' | sort -V | tr '\n' ' ' | sed 's/ $//')

    local first_pair=$(echo "$HW_CONN_PAIRS" | awk '{print $1}')
    HW_TEST_PORT_PRIMARY=${first_pair%:*}
    HW_TEST_PORT_SECONDARY=${first_pair#*:}

    local pair_count=$(echo "$HW_CONN_PAIRS" | wc -w)
    echo "Found $pair_count connection pair(s):"
    for pair in $HW_CONN_PAIRS; do
        echo "  $pair"
    done
    echo ""

    return 0
}

# Interactive connection setup
interactive_setup() {
    echo -e "${BLUE}Interactive Connection Setup${NC}"
    echo ""
    echo "Available channels: $HW_ALL_CHANNELS"
    echo ""
    echo "Enter connections in format: TX:RX (e.g., can0:can6)"
    echo "Enter 'done' when finished, 'auto' for auto-discovery"
    echo ""

    HW_CONN_PAIRS=""

    while true; do
        echo -n "Add connection (or 'done'/'auto'): "
        read input

        if [ "$input" = "done" ]; then
            break
        elif [ "$input" = "auto" ]; then
            auto_discover_connections
            return 0
        fi

        if [[ ! "$input" =~ ^can[0-9]+:can[0-9]+$ ]]; then
            log_error "Invalid format. Use TX:RX (e.g., can0:can6)"
            continue
        fi

        local tx=${input%:*}
        local rx=${input#*:}

        if [[ ! " $HW_ALL_CHANNELS " == *" $tx "* ]]; then
            log_error "Port $tx not found"
            continue
        fi

        if [[ ! " $HW_ALL_CHANNELS " == *" $rx "* ]]; then
            log_error "Port $rx not found"
            continue
        fi

        local duplicate=false
        for pair in $HW_CONN_PAIRS; do
            local existing_tx=${pair%:*}
            local existing_rx=${pair#*:}
            if [ "$tx" = "$existing_tx" ] || [ "$tx" = "$existing_rx" ] || \
               [ "$rx" = "$existing_tx" ] || [ "$rx" = "$existing_rx" ]; then
                log_warn "Port already in use ($pair)"
                duplicate=true
                break
            fi
        done

        if [ "$duplicate" = true ]; then
            echo -n "Add anyway? (y/N): "
            read confirm
            if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
                continue
            fi
        fi

        if [ -n "$HW_CONN_PAIRS" ]; then
            HW_CONN_PAIRS="$HW_CONN_PAIRS $input"
        else
            HW_CONN_PAIRS="$input"
        fi

        log_success "Added: $input"
        echo ""
    done

    if [ -n "$HW_CONN_PAIRS" ]; then
        local first_pair=$(echo "$HW_CONN_PAIRS" | cut -d' ' -f1)
        HW_TEST_PORT_PRIMARY=${first_pair%:*}
        HW_TEST_PORT_SECONDARY=${first_pair#*:}
    fi

    return 0
}

show_current_config() {
    echo -e "${BLUE}Current Configuration${NC}"
    echo "================================"
    echo ""
    echo "Total Devices:     ${HW_DEVICE_COUNT:-Unknown}"
    echo "Total Channels:    ${HW_TOTAL_CHANNELS:-Unknown}"
    echo "All Channels:      ${HW_ALL_CHANNELS:-None}"
    echo ""
    echo "Connection Pairs:"
    if [ -n "$HW_CONN_PAIRS" ]; then
        for pair in $HW_CONN_PAIRS; do
            local tx=${pair%:*}
            local rx=${pair#*:}
            echo "  $tx <-> $rx"
        done
    else
        echo "  (none)"
    fi
    echo ""
    echo "Primary Test Pair: ${HW_TEST_PORT_PRIMARY:-None} <-> ${HW_TEST_PORT_SECONDARY:-None}"
    echo ""
}

# Generate config files (bash + JSON)
generate_config_files() {
    echo -e "${BLUE}Generating configuration files...${NC}"
    echo ""

    local timestamp=$(date '+%Y-%m-%d')
    local timestamp_full=$(date '+%Y-%m-%d %H:%M:%S')

    # Generate bash config
    cat > "$PROJECT_DIR/hardware_config.conf" << EOF
################################################################################
# BMCAN Hardware Configuration File
#
# This file contains:
#   - All available CAN channel information
#   - Physical connection mapping
#   - Device grouping
#   - Recommended test port pairs
#
# Generated: $timestamp
# Last Updated: $timestamp_full
################################################################################

################################################################################
# Device Information
################################################################################

# Total USB devices detected
HW_DEVICE_COUNT=$HW_DEVICE_COUNT

# Total available channels
HW_TOTAL_CHANNELS=$HW_TOTAL_CHANNELS
HW_ALL_CHANNELS="$HW_ALL_CHANNELS"

################################################################################
# Physical Connection Matrix
# Format: TX_PORT:RX_PORT (bidirectional verified)
################################################################################

# Connection pairs
HW_CONN_PAIRS="$HW_CONN_PAIRS"

# Primary test ports (recommended for most tests)
HW_TEST_PORT_PRIMARY="$HW_TEST_PORT_PRIMARY"
HW_TEST_PORT_SECONDARY="$HW_TEST_PORT_SECONDARY"

################################################################################
# Channel Default Settings
################################################################################

# Default bitrate
HW_DEFAULT_BITRATE="500000"

# Default termination (ohms)
HW_DEFAULT_TERMINATION="120"

# Default termination options
HW_TERMINATION_OPTIONS="0 60 120 65535"

# Supported modes
HW_SUPPORTED_MODES="normal"
HW_UNSUPPORTED_MODES="loopback listen-only"

################################################################################
# Per-Channel Connection Mapping
################################################################################

EOF

    for pair in $HW_CONN_PAIRS; do
        local tx=${pair%:*}
        local rx=${pair#*:}
        local tx_num=${tx#can}
        local rx_num=${rx#can}

        cat >> "$PROJECT_DIR/hardware_config.conf" << EOF
# $tx <-> $rx
HW_CAN${tx_num}_CONNECTED_TO="$rx"
HW_CAN${tx_num}_AVAILABLE="true"
HW_CAN${tx_num}_TEST_PAIR="$pair"

HW_CAN${rx_num}_CONNECTED_TO="$tx"
HW_CAN${rx_num}_AVAILABLE="true"
HW_CAN${rx_num}_TEST_PAIR="$pair"

EOF
    done

    log_success "Generated: hardware_config.conf"

    # Generate JSON config
    local json_pairs="["
    local first=true
    for pair in $HW_CONN_PAIRS; do
        if [ "$first" = true ]; then
            json_pairs="$json_pairs{\"tx\":\"${pair%:*}\",\"rx\":\"${pair#*:}\",\"bidirectional\":true,\"quality\":\"100%\"}"
            first=false
        else
            json_pairs="$json_pairs,{\"tx\":\"${pair%:*}\",\"rx\":\"${pair#*:}\",\"bidirectional\":true,\"quality\":\"100%\"}"
        fi
    done
    json_pairs="$json_pairs]"

    cat > "$PROJECT_DIR/hardware_config.json" << EOF
{
  "generated": "$timestamp",
  "last_updated": "$timestamp_full",
  "total_channels": $HW_TOTAL_CHANNELS,
  "all_channels": [$(echo "$HW_ALL_CHANNELS" | sed 's/ /","/g' | sed 's/^/"/;s/$/"/')],
  "device_count": $HW_DEVICE_COUNT,
  "connection_pairs": $json_pairs,
  "default_config": {
    "bitrate": 500000,
    "termination": 120
  },
  "test_ports": {
    "primary": {
      "tx": "$HW_TEST_PORT_PRIMARY",
      "rx": "$HW_TEST_PORT_SECONDARY"
    }
  },
  "supported_modes": ["normal"],
  "unsupported_modes": ["loopback", "listen-only"]
}
EOF

    log_success "Generated: hardware_config.json"
    echo ""

    return 0
}

# Verify configuration by testing each pair
verify_config() {
    echo -e "${BLUE}Verifying new configuration...${NC}"
    echo ""

    source "$PROJECT_DIR/hardware_config.conf" 2>/dev/null || return 1

    echo "Testing connection pairs:"
    echo ""

    local all_ok=true
    for pair in $HW_CONN_PAIRS; do
        local tx=${pair%:*}
        local rx=${pair#*:}

        echo -n "  $tx -> $rx: "

        sudo ip link set $tx down 2>/dev/null || true
        sudo ip link set $rx down 2>/dev/null || true
        sleep 0.1
        sudo ip link set $tx up type can bitrate 500000 termination 120 2>/dev/null
        sudo ip link set $rx up type can bitrate 500000 termination 120 2>/dev/null
        sleep 0.2

        timeout 1 candump $rx > /tmp/verify_test.log 2>/dev/null &
        local pid=$!
        sleep 0.15
        sudo cansend $tx 123#DEADBEEF >/dev/null 2>&1
        sleep 0.3
        pkill -P $pid 2>/dev/null || true
        kill $pid 2>/dev/null || true
        wait $pid 2>/dev/null || true

        if [ -s /tmp/verify_test.log ]; then
            log_success "OK"
        else
            log_fail "FAILED"
            all_ok=false
        fi
        rm -f /tmp/verify_test.log
    done

    echo ""

    if [ "$all_ok" = true ]; then
        log_success "All connections verified!"
        return 0
    else
        log_error "Some connections failed!"
        echo "Please check physical connections and try again."
        return 1
    fi
}

save_with_backup() {
    local file=$1

    if [ -f "$file" ]; then
        local backup="${file}.bak.$(date +%Y%m%d_%H%M%S)"
        cp "$file" "$backup"
        log_warn "Backed up: $backup"
    fi
}

################################################################################
# Main
################################################################################

main() {
    cd "$PROJECT_DIR" || exit 1

    show_banner

    local mode="interactive"
    local skip_verify=false

    while [ $# -gt 0 ]; do
        case $1 in
            --auto)
                mode="auto"
                shift
                ;;
            --set)
                mode="direct"
                shift
                HW_CONN_PAIRS=""
                while [ $# -gt 0 ] && [[ "$1" =~ ^can[0-9]+:can[0-9]+$ ]]; do
                    if [ -n "$HW_CONN_PAIRS" ]; then
                        HW_CONN_PAIRS="$HW_CONN_PAIRS $1"
                    else
                        HW_CONN_PAIRS="$1"
                    fi
                    shift
                done
                ;;
            --skip-verify)
                skip_verify=true
                shift
                ;;
            --help|-h)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --auto                 Auto-discover connections"
                echo "  --set TX:RX [TX:RX]...  Set connections directly"
                echo "  --skip-verify          Skip connection verification"
                echo "  --help, -h             Show this help"
                echo ""
                echo "Examples:"
                echo "  $0                 # Interactive mode"
                echo "  $0 --auto          # Auto-discover"
                echo "  $0 --set can0:can6 can1:can7"
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                echo "Use --help for usage"
                exit 1
                ;;
        esac
    done

    if ! scan_devices; then
        exit 1
    fi

    case $mode in
        auto)
            auto_discover_connections
            ;;
        direct)
            if [ -z "$HW_CONN_PAIRS" ]; then
                log_error "No connections provided"
                exit 1
            fi
            local first_pair=$(echo "$HW_CONN_PAIRS" | cut -d' ' -f1)
            HW_TEST_PORT_PRIMARY=${first_pair%:*}
            HW_TEST_PORT_SECONDARY=${first_pair#*:}
            ;;
        interactive)
            interactive_setup
            ;;
    esac

    show_current_config

    if [ "$mode" = "interactive" ]; then
        echo -n "Save this configuration? (y/N): "
        read confirm
        if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
            echo "Aborted."
            exit 0
        fi
    fi

    echo ""
    save_with_backup "$PROJECT_DIR/hardware_config.conf"
    save_with_backup "$PROJECT_DIR/hardware_config.json"

    generate_config_files

    if [ "$skip_verify" = false ]; then
        if ! verify_config; then
            echo ""
            log_warn "Configuration generated but verification failed."
            echo "Files saved anyway. You can:"
            echo "  1. Check physical connections"
            echo "  2. Run: bash tools/update_hardware_config.sh --auto"
            echo "  3. Or skip verification next time: --skip-verify"
            exit 1
        fi
    fi

    echo ""
    echo "=========================================="
    log_success "Configuration Updated Successfully!"
    echo "=========================================="
    echo ""
    echo "Files updated:"
    echo "  - hardware_config.conf"
    echo "  - hardware_config.json"
    echo ""
    echo "Next steps:"
    echo "  1. Run tests: bash run_all_tests.sh"
    echo "  2. Or verify: bash tools/verify_hardware.sh"
    echo ""
}

main "$@"
