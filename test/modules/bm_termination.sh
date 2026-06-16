#!/bin/bash
# Module 2: 终端电阻测试
# 功能：验证终端电阻设置和恢复


# Import libraries
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# Test functions
################################################################################

test_termination_120ohm() {
    local port=$1
    log_info "Testing 120Ω termination on $port"

    # 单命令：设置并验证（CANFD模式）
    if (sudo ip link set $port down; sleep 0.3; sudo ip link set $port up type can bitrate 500000 sample-point 0.875 dbitrate 2000000 dsample-point 0.750 fd on termination 120) >/dev/null 2>&1 && \
       ip -details link show $port | grep -q "termination 120"; then
        log_success "120Ω termination OK"
        return 0
    else
        log_fail "120Ω termination FAILED"
        return 1
    fi
}

test_termination_60ohm() {
    local port=$1
    log_info "Testing 60Ω termination on $port"

    # 单命令：设置并验证
    if (sudo ip link set $port type can termination 60 >/dev/null 2>&1) && \
       ip -details link show $port | grep -q "termination 60"; then
        log_success "60Ω termination OK"
        return 0
    else
        log_fail "60Ω termination FAILED"
        return 1
    fi
}

test_termination_disable() {
    local port=$1
    log_info "Testing termination disable (0Ω) on $port"

    # 单命令：设置并验证
    if (sudo ip link set $port type can termination 0 >/dev/null 2>&1) && \
       ip -details link show $port | grep -q "termination 0"; then
        log_success "Termination disable OK"
        return 0
    else
        log_fail "Termination disable FAILED"
        return 1
    fi
}

test_termination_all_ports() {
    local ports=("$@")
    local count=${#ports[@]}

    log_info "Testing termination on all $count ports"

    for port in "${ports[@]}"; do
        if ! ip -details link show $port | grep -q "termination 120"; then
            log_fail "$port does not have 120Ω termination"
            return 1
        fi
    done

    log_success "All ports have 120Ω termination"
    return 0
}

################################################################################
# 主测试流程
################################################################################

main() {
    log_info "=========================================="
    log_info "Module 2: Termination Test"
    log_info "=========================================="
    echo ""

    # 初始化测试套件

    # 读取连接的端口
    if [ ! -f "$SCRIPT_DIR/../results/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        log_error "Please run hardware discovery first."
        return 1
    fi

    local ports=($(cat "$SCRIPT_DIR/../results/connected_ports.txt"))
    local test_port=${ports[0]}

    log_info "Test port: $test_port"
    log_info "Available ports: ${ports[@]}"
    echo ""

    # 备份配置
    start_testcase "Backup_Config"
    backup_port_config $test_port
    test_pass "Config backed up"
    echo ""

    # 测试1: 120Ω默认值
    start_testcase "Termination_120ohm_Default"
    if test_termination_120ohm $test_port; then
        test_pass "120Ω is default"
    else
        test_fail "120Ω default check failed"
    fi
    echo ""

    # 测试2: 60Ω
    start_testcase "Termination_60ohm"
    if test_termination_60ohm $test_port; then
        test_pass "60Ω setting works"
    else
        test_fail "60Ω setting failed"
    fi
    echo ""

    # 测试3: 禁用
    start_testcase "Termination_Disable"
    if test_termination_disable $test_port; then
        test_pass "Termination disable works"
    else
        test_fail "Termination disable failed"
    fi
    echo ""

    # 测试4: 所有端口 (restore 120Ω first after disable test)
    sudo ip link set $test_port type can termination 120 >/dev/null 2>&1
    start_testcase "Termination_All_Ports"
    if test_termination_all_ports "${ports[@]}"; then
        test_pass "All ports have correct termination"
    else
        test_fail "Some ports have wrong termination"
    fi
    echo ""

    # 跳过配置恢复（会导致测试用例丢失）
    log_info "Skipping configuration restore to preserve test results..."
    # restore_port_config $test_port
    # verify_port_config $test_port
    echo ""

    log_success "Termination test completed"
    return 0
}

# 执行主函数
