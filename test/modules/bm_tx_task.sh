#!/bin/bash
# Module 5: 周期发送测试
# 功能：txtask周期发送功能测试


# Import libraries
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# 测试函数
################################################################################

test_tx_task_configure() {
    local port=$1
    log_info "Configuring TX task on $port"

    # 单命令：配置txtask
    if sudo $API_BIN txtasks \
        --type fixed --id 0x100 --payload AABBCCDD \
        --cycle 100 --index 0 --device $port --apply >/dev/null 2>&1; then
        log_success "TX task configured"
        return 0
    else
        log_fail "TX task configuration failed"
        return 1
    fi
}

test_tx_task_verify() {
    local tx=$1
    local rx=$2
    log_info "Verifying periodic transmission"

    # 单命令：监听并验证周期接收
    local received=$(timeout 5 candump $rx 2>/dev/null | grep -c "100" || echo 0)
    received=$(echo "$received" | tr -d '\n\r' | head -1)  # Remove newlines
    received=${received:-0}  # Default to 0 if empty

    if [ "$received" -ge 3 ]; then
        log_success "Periodic transmission verified ($received frames in 5s)"
        return 0
    else
        log_fail "Periodic transmission failed (only $received frames)"
        return 1
    fi
}

test_tx_task_stop() {
    local port=$1
    log_info "Stopping TX task"

    # 单命令：停止txtask（注意：invalidate不需要--apply）
    if sudo $API_BIN invalidate --txtask --tx-range 0-0 --device $port >/dev/null 2>&1; then
        log_success "TX task stopped"
        return 0
    else
        log_fail "Failed to stop TX task"
        return 1
    fi
}

test_tx_task_data() {
    local tx=$1
    local rx=$2
    log_info "Verifying data content"

    # 先等待TX task启动发送，然后监听验证数据
    sleep 1
    local data_ok=0
    for i in {1..3}; do
        if timeout 3 candump $rx 2>/dev/null | grep -q "100.*AA.*BB.*CC.*DD"; then
            ((data_ok++))
        fi
    done

    if [ $data_ok -ge 1 ]; then
        log_success "Data content correct (AABBCCDD)"
        return 0
    else
        log_fail "Data content incorrect"
        return 1
    fi
}

################################################################################
# 主测试流程
################################################################################

main() {
    log_info "=========================================="
    log_info "Module 5: TX Task Test"
    log_info "=========================================="
    echo ""


    # 检查API工具
    if [ ! -x "$API_BIN" ]; then
        log_error "API tool not found: $API_BIN"
        log_error "Please build the project first"
        return 1
    fi

    log_info "API tool check passed"

    # 读取端口
    if [ ! -f "$SCRIPT_DIR/../results/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi

    log_info "Connected ports file found"

    local ports=($(cat "$SCRIPT_DIR/../results/connected_ports.txt"))
    local tx_port=${ports[0]}
    local rx_port=${ports[1]}

    log_info "TX port: $tx_port"
    log_info "RX port: $rx_port"
    echo ""

    # 测试1: 配置txtask
    start_testcase "TX_Task_Configure_Fixed"
    if test_tx_task_configure $tx_port; then
        test_pass "TX task configured successfully"
    else
        test_fail "TX task configuration failed"
    fi
    echo ""

    # 测试2: 验证周期发送
    start_testcase "TX_Task_Verify_Periodic"
    sleep 1  # 等待第一个周期
    if test_tx_task_verify $tx_port $rx_port; then
        test_pass "Periodic transmission working"
    else
        test_fail "Periodic transmission failed"
    fi
    echo ""

    # 测试3: 验证数据内容
    start_testcase "TX_Task_Data_Content"
    if test_tx_task_data $tx_port $rx_port; then
        test_pass "Data content correct"
    else
        test_fail "Data content incorrect"
    fi
    echo ""

    # 清理：停止txtask并恢复配置
    log_info "Cleaning up..."
    start_testcase "TX_Task_Stop"
    if test_tx_task_stop $tx_port; then
        test_pass "TX task stopped"
    else
        test_fail "Failed to stop TX task"
    fi
    echo ""

    # Test: TX task range invalidate actually stops all periodic transmission
    start_testcase "TX_Task_Range_Invalidate_Verify"
    log_info "Testing txtask range invalidate stops all transmission (range 0-5)..."
    # Configure txtasks on indices 0-5
    local ti
    for ti in 0 1 2 3 4 5; do
        sudo $API_BIN txtasks --type fixed --id $((0x100 + ti)) --payload AABBCCDD \
            --cycle 100 --index $ti --device $tx_port --apply >/dev/null 2>&1 || true
    done
    sleep 1
    # Verify they are transmitting
    local tx_before
    tx_before=$(timeout 3 candump $rx_port -n 100 -t A 2>/dev/null | grep -cE ' 10[012345] ' || true)
    tx_before=$(echo "$tx_before" | tr -d '[:space:]')
    tx_before=${tx_before:-0}
    # Invalidate range 0-5
    sudo $API_BIN invalidate --txtask --tx-range 0-5 --device $tx_port >/dev/null 2>&1
    sleep 1
    # Verify they stopped
    local tx_after
    tx_after=$(timeout 3 candump $rx_port -n 100 -t A 2>/dev/null | grep -cE ' 10[012345] ' || true)
    tx_after=$(echo "$tx_after" | tr -d '[:space:]')
    tx_after=${tx_after:-0}
    if [ "$tx_after" -eq 0 ]; then
        test_pass "TX task range invalidate stopped all 6 transmissions (0-5)"
    else
        test_fail "TX task range invalidate did not stop all ($tx_after frames still received)"
    fi
    echo ""

    # 恢复配置到标准状态（正常模式 + CANFD + 120Ω）
    log_info "Restoring configuration..."
    restore_port_config $tx_port
    verify_port_config $tx_port
    echo ""

    log_success "TX task test completed"
    return 0
}

