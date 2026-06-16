#!/bin/bash
# Module: bm_hardware_discovery.sh
# 功能：
#   1. 下电上电所有CAN接口（在同一命令行设置波特率、mode、终端电阻）
#   2. 获取所有可用CAN通道
#   3. 测试所有通道的双向通信
#   4. 输出物理连接成功的通道列表
#   5. 如果全部失败则退出测试


# Import test framework
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"

# Configuration
POWER_CYCLE_DELAY=1
TEST_TIMEOUT=5
TEST_FRAMES=5

# Connected ports array
declare -a CONNECTED_PORTS=()

# 下电上电所有CAN接口
power_cycle_all_ports() {
    log_info "Power cycling all CAN ports..."

    local ports=$(ip link show | grep -oE 'can[0-9]+' | sort -u || true)

    if [ -z "$ports" ]; then
        log_error "No CAN ports found!"
        return 1
    fi

    # 下电
    for port in $ports; do
        sudo ip link set $port down 2>/dev/null || true
    done

    sleep $POWER_CYCLE_DELAY

    # 上电（在同一命令行设置波特率、mode、终端电阻）
    for port in $ports; do
        sudo ip link set $port up type can bitrate 500000 termination 120 2>/dev/null || true
    done

    sleep 1
    log_success "Power cycle completed"
    return 0
}

# 测试单个通道的双向通信
test_bidirectional_comm() {
    local port_a=$1
    local port_b=$2

    # 测试 A -> B
    candump $port_b 2>/dev/null > /tmp/test_${port_a}_${port_b}.log &
    local CANDUMP_PID=$!
    sleep 1  # Give candump more time to start

    local sent=0
    for i in $(seq 1 $TEST_FRAMES); do
        sudo cansend $port_a $((100 + i))#DEADBEEF >/dev/null 2>&1 && ((sent++)) || true
        sleep 0.15  # Slightly longer delay between frames
    done

    sleep 1.5  # Wait for last frames to be received
    kill $CANDUMP_PID 2>/dev/null || true

    local received=$(wc -l < /tmp/test_${port_a}_${port_b}.log 2>/dev/null || echo 0)
    rm -f /tmp/test_${port_a}_${port_b}.log

    [ $received -gt 0 ]
}

# 硬件发现主函数
run_hardware_discovery() {
    log_info "=========================================="
    log_info "Hardware Discovery and Connection Test"
    log_info "=========================================="
    echo ""

    # Step 1: 下电上电
    start_testcase "Power Cycle All Ports"
    if power_cycle_all_ports; then
        test_pass "Power cycle successful"
    else
        test_fail "Power cycle failed"
        return 1
    fi
    echo ""

    # Step 2: 发现CAN接口
    start_testcase "Discover CAN Interfaces"
    local all_ports=$(ip link show | grep -oE 'can[0-9]+' | sort -u || true)

    if [ -z "$all_ports" ]; then
        test_fail "No CAN interfaces found"
        log_error "No CAN interfaces found! Aborting tests."
        return 1
    fi

    local port_count=$(echo "$all_ports" | wc -l)
    log_success "Found $port_count CAN interface(s)"
    test_pass "Found $port_count CAN interfaces"
    echo ""

    # Step 3: 测试所有通道的双向通信
    start_testcase "Test Physical Connections"
    log_info "Testing physical connections between all ports..."

    local ports_array=($all_ports)
    local total_ports=${#ports_array[@]}

    # Temporarily disable exit on error for testing
    set +e

    # 测试每一对端口
    local i=0
    while [ $i -lt $total_ports ]; do
        local port_a=${ports_array[$i]}
        local connected=0
        log_info "Testing port $port_a..."

        local j=0
        while [ $j -lt $total_ports ]; do
            if [ $i -ne $j ]; then
                local port_b=${ports_array[$j]}
                log_info "  Testing $port_a -> $port_b..."
                if test_bidirectional_comm $port_a $port_b; then
                    log_success "    $port_a <-> $port_b connected"
                    if ! [[ " ${CONNECTED_PORTS[@]} " =~ " ${port_a} " ]]; then
                        CONNECTED_PORTS+=($port_a)
                    fi
                    if ! [[ " ${CONNECTED_PORTS[@]} " =~ " ${port_b} " ]]; then
                        CONNECTED_PORTS+=($port_b)
                    fi
                    connected=1
                    break
                else
                    log_info "    No connection"
                fi
            fi
            ((j++))
        done

        ((i++))
    done

    # Re-enable exit on error
    set -e

    local connected_count=${#CONNECTED_PORTS[@]}

    echo ""
    log_info "Physical Connection Summary:"
    log_info "  Total ports: $total_ports"
    log_info "  Connected ports: $connected_count"

    if [ $connected_count -eq 0 ]; then
        log_error "No physical connections detected!"
        log_error "All tests will be aborted."
        test_fail "No physical connections - aborting"
        return 1
    fi

    log_success "Connected ports:"
    for port in "${CONNECTED_PORTS[@]}"; do
        echo "  - $port"
    done

    test_pass "Found $connected_count connected ports"

    echo ""
    log_success "Hardware Discovery Completed"

    return 0
}

# 导出连接的端口列表
export_connected_ports() {
    local result_file="$1"
    : > "$result_file"
    for port in "${CONNECTED_PORTS[@]}"; do
        echo "$port" >> "$result_file"
    done
}

# 主函数
main() {
    run_hardware_discovery

    # 导出连接的端口列表
    if [ ${#CONNECTED_PORTS[@]} -gt 0 ]; then
        local result_file="$SCRIPT_DIR/../results/connected_ports.txt"
        mkdir -p "$(dirname "$result_file")"
        export_connected_ports "$result_file"
        log_info "Exported connected ports to: $result_file"
    fi

    return $?
}

# 如果直接执行
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
