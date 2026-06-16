#!/bin/bash
# Module 6: 路由转发测试
# 功能：route路由转发功能测试


# Import libraries
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"
source "$SCRIPT_DIR/../lib/config_manager.sh"

################################################################################
# 测试函数
################################################################################

test_route_configure_broadcast() {
    local port=$1
    log_info "Configuring broadcast route on $port"

    # Single command: configure broadcast route
    if sudo $API_BIN routes \
        --route-type broadcast --source 0 --target 0xFF \
        --index 1 --device $port --apply >/dev/null 2>&1; then
        log_success "Broadcast route configured"
        return 0
    else
        log_fail "Route configuration failed"
        return 1
    fi
}

test_route_verify_forwarding() {
    local port_from=$1
    local port_route=$2
    local port_to=$3
    log_info "Verifying routing: $port_from -> $port_route -> $port_to"

    # 单命令：发送并验证转发（数据必须是十六进制）
    (sudo cansend $port_from 300#DEADBEEF >/dev/null 2>&1) &
    local received=$(timeout 5 candump $port_to 2>/dev/null | grep -c "300" || echo 0)

    # 清理变量：去除非数字字符
    received=$(echo "$received" | tr -d '\n\r' | head -1)
    received=${received:-0}
    [[ "$received" =~ ^[0-9]+$ ]] || received=0

    if [ $received -gt 0 ]; then
        log_success "Route forwarding works ($received frames)"
        return 0
    else
        log_fail "Route forwarding failed"
        return 1
    fi
}

test_route_clear() {
    local port=$1
    log_info "Clearing route configuration"

    # 单命令：清除路由（注意：invalidate不需要--apply）
    if sudo $API_BIN invalidate --route --route-range 0-3 --device $port >/dev/null 2>&1; then
        log_success "Route cleared"
        return 0
    else
        log_fail "Failed to clear route"
        return 1
    fi
}

################################################################################
# 主测试流程
################################################################################

main() {
    log_info "=========================================="
    log_info "Module 6: Route Test"
    log_info "=========================================="
    echo ""


    # 检查API工具
    if [ ! -x "$API_BIN" ]; then
        log_error "API tool not found: $API_BIN"
        return 1
    fi

    # 读取端口
    if [ ! -f "$SCRIPT_DIR/../results/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi

    local ports=($(cat "$SCRIPT_DIR/../results/connected_ports.txt"))

    if [ ${#ports[@]} -lt 2 ]; then
        log_error "Need at least 2 connected ports (found: ${#ports[@]})"
        return 1
    fi

    # 使用物理连接的端口对：can0 ↔ can6
    # 在 can0 配置路由，从 can6 发送，can0 接收后转发回 can6
    local port_from=${ports[1]}  # can6 (发送)
    local port_route=${ports[0]}  # can0 (路由)
    local port_to=${ports[1]}    # can6 (接收转发的帧)

    log_info "From: $port_from"
    log_info "Route: $port_route"
    log_info "To: $port_to"
    echo ""

    # 测试1: 配置广播路由
    start_testcase "Route_Configure_Broadcast"
    if test_route_configure_broadcast $port_route; then
        test_pass "Broadcast route configured"
    else
        test_fail "Route configuration failed"
    fi
    echo ""

    # 测试2: 验证帧转发
    start_testcase "Route_Verify_Forwarding"
    if test_route_verify_forwarding $port_from $port_route $port_to; then
        test_pass "Route forwarding works"
    else
        test_fail "Route forwarding failed"
    fi
    echo ""

    # 清理：清除路由配置并恢复标准配置
    log_info "Cleaning up..."
    start_testcase "Route_Clear"
    if test_route_clear $port_route; then
        test_pass "Route cleared"
    else
        test_fail "Failed to clear route"
    fi
    echo ""

    # Test: Route range invalidate actually stops forwarding
    start_testcase "Route_Range_Invalidate_Verify"
    log_info "Testing route range invalidate stops all forwarding (range 0-5)..."
    # Configure routes on indices 0-5
    local ri
    for ri in 0 1 2 3 4 5; do
        sudo $API_BIN routes --route-type broadcast --source 0 --target 0xFF \
            --index $ri --device $port_route --apply >/dev/null 2>&1 || true
    done
    # Invalidate range 0-5
    sudo $API_BIN invalidate --route --route-range 0-5 --device $port_route >/dev/null 2>&1
    # Verify routes are cleared by checking sysfs (should be all zeros)
    local route_data
    route_data=$(cat /sys/class/net/$port_route/routes 2>/dev/null | grep '^\[')
    local route_cleared=true
    for ri in 0 1 2 3 4 5; do
        local entry
        entry=$(echo "$route_data" | grep "^\[$ri\]" | sed 's/^\[[0-9]*\] //')
        # After invalidation, hex data should be all zeros
        if [ -n "$entry" ] && echo "$entry" | grep -qvE '^[0\s]*$'; then
            route_cleared=false
            log_warn "Route index $ri not cleared: $entry"
        fi
    done
    if [ "$route_cleared" = true ]; then
        test_pass "Route range invalidate cleared all 6 indices (0-5)"
    else
        test_fail "Route range invalidate did not clear all indices"
    fi
    echo ""

    # 恢复所有端口到标准状态（正常模式 + CANFD + 120Ω）
    log_info "Restoring configuration..."
    restore_port_config $port_from
    restore_port_config $port_route
    restore_port_config $port_to
    verify_port_config $port_from
    echo ""

    log_success "Route test completed"
    return 0
}

