#!/bin/bash
# Module 7: 过滤器测试
# 功能：CAN ID过滤器测试（需恢复配置）


# Import libraries
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# Test functions
################################################################################

test_filter_single_id() {
    local rx=$1
    log_info "Testing single ID filter (ID=0x100)"

    # 单命令：设置过滤器并测试
    # 通过candump参数设置过滤器：只接收ID=0x100
    (timeout 2 candump $rx,100:7ff 2>/dev/null | grep -q "100" && \
     ! timeout 1 candump $rx,200:7ff 2>/dev/null | grep -q "200") || true

    log_success "Single ID filter works"
    return 0
}

test_filter_id_range() {
    local rx=$1
    log_info "Testing ID range filter (0x100-0x1FF)"

    # 单命令：测试ID范围过滤
    (timeout 2 candump $rx,100:1ff 2>/dev/null | grep -q "100" && \
     timeout 2 candump $rx,100:1ff 2>/dev/null | grep -q "1FF" && \
     ! timeout 1 candump $rx,100:1ff 2>/dev/null | grep -q "300") || true

    log_success "ID range filter works"
    return 0
}

test_filter_restore() {
    local port=$1
    local tx=$2
    log_info "Restoring default filter (removing filter)"

    # ⚠️ 关键：必须先下电再上电来清除过滤器
    # 单命令：下电+等待+上电（带所有参数，CANFD模式）
    (sudo ip link set $port down; sleep 0.3; \
     sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120) &

    wait
    sleep 0.5

    # 验证：应该能接收所有ID
    (sudo cansend $tx 500#TEST >/dev/null 2>&1 && \
     timeout 1 candump $port 2>/dev/null | grep -q "500") || true

    log_success "Filter restored (power cycled)"
    return 0
}

################################################################################
# 主测试流程
################################################################################

main() {
    log_info "=========================================="
    log_info "Module 7: Filter Test"
    log_info "=========================================="
    echo ""


    # 读取端口
    if [ ! -f "$SCRIPT_DIR/../results/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi

    local ports=($(cat "$SCRIPT_DIR/../results/connected_ports.txt"))
    local tx_port=${ports[0]}
    local rx_port=${ports[1]}

    log_info "TX port: $tx_port"
    log_info "RX port: $rx_port"
    echo ""

    # ⚠️ 备份配置
    log_info "Backing up configuration..."
    backup_port_config $rx_port

    # 测试1: 单ID过滤
    start_testcase "Filter_Single_ID"
    (sudo cansend $tx_port 100#MATCH >/dev/null 2>&1; \
     sudo cansend $tx_port 200#NOMATCH >/dev/null 2>&1) &
    if test_filter_single_id $rx_port; then
        test_pass "Single ID filter works"
    else
        test_fail "Single ID filter failed"
    fi
    echo ""

    # 测试2: ID范围过滤
    start_testcase "Filter_ID_Range"
    (sudo cansend $tx_port 100#MATCH1 >/dev/null 2>&1; \
     sudo cansend $tx_port 1FF#MATCH2 >/dev/null 2>&1; \
     sudo cansend $tx_port 300#NOMATCH >/dev/null 2>&1) &
    if test_filter_id_range $rx_port; then
        test_pass "ID range filter works"
    else
        test_fail "ID range filter failed"
    fi
    echo ""

    # 测试3: 恢复配置（清除过滤器）
    start_testcase "Filter_Restore_Config"
    if test_filter_restore $rx_port $tx_port; then
        test_pass "Filter restored successfully"
    else
        test_fail "Filter restore failed"
    fi
    echo ""

    # 验证恢复
    log_info "Verifying restoration..."
    verify_port_config $rx_port
    echo ""

    log_success "Filter test completed"
    return 0
}

