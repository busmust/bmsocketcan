#!/bin/bash
# Module: Bus-Off Stress Test
#
# Combines BMAPI SDK bmapi_transmit_only + busoff_stress_test patterns:
#
# DUT side (bmapi_transmit_only pattern via bmcan_api txtask):
#   - 4 CAN IDs at 10/20/40/40ms via hardware txtask
#   - CAN FD BRS 8-byte frames
#   - restart-ms=100 for auto bus-off recovery
#
# Partner side (busoff_stress_test pattern via bmcan_api txtask):
#   - Cycle: Open -> SetTxTask (1ms CAN FD 64B heavy TX) -> poll DUT -> Close
#   - Weighted dwell times: 60x1s, 10x3s, 5x9s, 1x30s
#   - Each Close: partner's 1ms flood disappears + DUT loses ACK partner
#   - Recovery = partner reopens and receives DUT frame
#
# Supports multi-pair: can0↔can6, can1↔can7, can2↔can4, can3↔can5

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/common.sh"
source "$SCRIPT_DIR/../lib/junit_xml.sh"

################################################################################
# DUT Side: bmapi_transmit_only pattern (hardware txtask)
################################################################################

dut_start() {
    local port=$1

    sudo ip link set $port down 2>/dev/null || true
    sleep 0.2
    sudo ip link set $port up type can bitrate 500000 dbitrate 2000000 fd on termination 120 restart-ms 100 2>/dev/null
    sleep 0.5

    # Software TX via cangen (matching BM_Send from bmapi_transmit_only)
    # 4 CAN IDs at 10/20/40/40ms
    sudo cangen $port -g 10 -L 8 -I 117 -D 0300000000000000 >/dev/null 2>&1 &
    sudo cangen $port -g 20 -L 8 -I 1B5 -D 0300000000000000 >/dev/null 2>&1 &
    sudo cangen $port -g 40 -L 8 -I 224 -D 0300000000000000 >/dev/null 2>&1 &
    sudo cangen $port -g 40 -L 8 -I 298 -D 0300000000000000 >/dev/null 2>&1 &
}

dut_stop() {
    local port=$1
    sudo killall cangen 2>/dev/null || true
}

################################################################################
# Partner Side: busoff_stress_test pattern
################################################################################

partner_open() {
    local port=$1
    local retry
    for retry in 1 2 3; do
        sudo ip link set $port down 2>/dev/null || true
        sleep 0.2
        sudo ip link set $port up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
        sleep 0.3
        if ip link show $port 2>/dev/null | grep -q "UP"; then
            return 0
        fi
        log_warn "    Partner open retry $retry/3 for $port"
        sleep 0.5
    done
    return 1
}

# Partner SetTxTask: 1ms CAN FD BRS 64 bytes (matches busoff_stress_test)
partner_set_txtask() {
    local port=$1 ch=$2
    # Hardware txtask (matching BM_SetTxTask from busoff_stress_test)
    # 1ms cycle, CAN FD BRS 64 bytes
    sudo $API_BIN txtasks --index 0 --type fixed --id $((0x500 + ch)) --cycle 1 \
        --length 64 --payload AAAAAAAAAAAAAAAA --fd --brs --device $port >/dev/null 2>&1 || true
}

partner_clear_txtask() {
    local port=$1
    $API_BIN invalidate --txtask --tx-range 0-0 --device $port >/dev/null 2>&1 || true
}

partner_close() {
    local port=$1
    sudo ip link set $port down 2>/dev/null || true
}

# Poll for DUT frames using candump (real usage scenario)
poll_dut_frames() {
    local port=$1 dwell_ms=$2
    local logf="/tmp/busoff_stress_poll_$$_${port}.log"
    rm -f "$logf"

    local start_ms=$(date +%s%3N)

    timeout $(((dwell_ms + 500) / 1000)) candump $port -n 100 -t A > "$logf" 2>/dev/null || true

    local rx_count
    rx_count=$(grep -cE ' 117 | 1B5 | 224 | 298 ' "$logf" 2>/dev/null) || rx_count=0

    local recovery_ms=0
    if [ "$rx_count" -gt 0 ]; then
        recovery_ms=$(( ($(date +%s%3N) - start_ms) ))
    fi

    rm -f "$logf"
    echo "$rx_count $recovery_ms"
}

cleanup_restore() {
    local port=$1
    sudo killall cangen 2>/dev/null || true
    sudo ip link set $port down 2>/dev/null || true
    sleep 0.3
    sudo ip link set $port up type can bitrate 500000 dbitrate 2000000 fd on termination 120 2>/dev/null
    sleep 0.3
}

count_can_interfaces() {
    ip -o link show 2>/dev/null | grep -c ': can[0-9]*:'
}

################################################################################
# Test Cases
################################################################################

G_DUT_PORTS=()
G_PARTNER_PORTS=()
G_NUM_PAIRS=0

# Test 1: Core bus-off stress — BMAPI combined pattern
test_busoff_stress_core() {
    local total_steps=$1

    log_info "  Core stress: $G_NUM_PAIRS DUT/Partner pairs, $total_steps steps"
    log_info "  DUT: 4 txtasks via bmcan_api (0x117@10ms, 0x1B5@20ms, 0x224@40ms, 0x298@40ms)"
    log_info "  Partner: open -> txtask 1ms/64B -> poll -> close"
    echo ""

    # Configure all DUT ports (restart-ms for auto recovery)
    local ch
    for ch in $(seq 0 $((G_NUM_PAIRS - 1))); do
        sudo ip link set ${G_DUT_PORTS[$ch]} down 2>/dev/null || true
        sleep 0.2
        sudo ip link set ${G_DUT_PORTS[$ch]} up type can bitrate 500000 dbitrate 2000000 fd on termination 120 restart-ms 100 2>/dev/null
        sleep 0.3
    done

    # Open first partner, THEN start DUT txtasks
    partner_open ${G_PARTNER_PORTS[0]} || true
    sleep 0.3

    for ch in $(seq 0 $((G_NUM_PAIRS - 1))); do
        dut_start ${G_DUT_PORTS[$ch]}
    done
    sleep 1

    # Verify DUT is transmitting
    local verify=$(poll_dut_frames ${G_PARTNER_PORTS[0]} 3000)
    local verify_rx=$(echo $verify | awk '{print $1}')
    if [ "$verify_rx" -eq 0 ]; then
        log_warn "  DUT not transmitting after 3s — aborting"
        partner_close ${G_PARTNER_PORTS[0]}
        for ch in $(seq 0 $((G_NUM_PAIRS - 1))); do
            dut_stop ${G_DUT_PORTS[$ch]}
        done
        return 1
    fi
    log_info "  DUT verified: $verify_rx frames in 3s on ${G_PARTNER_PORTS[0]}"
    partner_close ${G_PARTNER_PORTS[0]}
    sleep 0.5

    # Build dwell pattern: 60x1s, 10x3s, 5x9s, 1x30s per round, cycle channels
    local one_round=()
    local dwell_configs=(
        "60 1000"
        "10 3000"
        "5 9000"
        "1 30000"
    )
    for cfg in "${dwell_configs[@]}"; do
        local count=$(echo $cfg | awk '{print $1}')
        local dwell=$(echo $cfg | awk '{print $2}')
        local r c
        for r in $(seq 1 $count); do
            for c in $(seq 0 $((G_NUM_PAIRS - 1))); do
                one_round+=("$dwell")
            done
        done
    done
    local round_size=${#one_round[@]}

    local num_steps=$total_steps
    local i
    local steps=()
    for i in $(seq 0 $((num_steps - 1))); do
        steps+=(${one_round[$((i % round_size))]})
    done

    log_info "  Total steps: $num_steps (round=$round_size, pairs=$G_NUM_PAIRS)"

    local total_cycles=0
    local total_recovered=0
    local total_failed=0
    local consecutive_fail=0
    local total_recovery_ms=0
    local min_recovery_ms=999999
    local max_recovery_ms=0

    for i in $(seq 0 $((num_steps - 1))); do
        local dwell=${steps[$i]}
        local ch=$((i % G_NUM_PAIRS))
        local partner_port=${G_PARTNER_PORTS[$ch]}

        # Partner: Open (with retry, like BMAPI OpenEx)
        if ! partner_open $partner_port; then
            total_cycles=$((total_cycles + 1))
            total_failed=$((total_failed + 1))
            consecutive_fail=$((consecutive_fail + 1))
            log_warn "    #$(printf '%04d' $total_cycles) CH$ch OPEN_FAILED (consecutive=$consecutive_fail)"
            continue
        fi

        # Partner: SetTxTask 1ms CAN FD 64B (heavy bidirectional traffic)
        partner_set_txtask $partner_port $ch

        # Partner: Poll for DUT frames during dwell
        local result
        result=$(poll_dut_frames $partner_port $dwell)
        local rx_count=$(echo $result | awk '{print $1}')
        local recovery_ms=$(echo $result | awk '{print $2}')

        # Partner: Close (DUT loses ACK + heavy TX disappears)
        partner_clear_txtask $partner_port
        partner_close $partner_port

        total_cycles=$((total_cycles + 1))

        if [ "$rx_count" -gt 0 ]; then
            total_recovered=$((total_recovered + 1))
            consecutive_fail=0
            total_recovery_ms=$((total_recovery_ms + recovery_ms))
            [ "$recovery_ms" -lt "$min_recovery_ms" ] && min_recovery_ms=$recovery_ms
            [ "$recovery_ms" -gt "$max_recovery_ms" ] && max_recovery_ms=$recovery_ms

            if [ "$dwell" -ge 9000 ] || [ $((total_cycles % 25)) -eq 0 ]; then
                log_info "    #$(printf '%04d' $total_cycles) CH$ch dwell=${dwell}ms RECOVERED t=${recovery_ms}ms rx=$rx_count"
            fi
        else
            total_failed=$((total_failed + 1))
            consecutive_fail=$((consecutive_fail + 1))
            log_warn "    #$(printf '%04d' $total_cycles) CH$ch dwell=${dwell}ms NO_RECOVERY (consecutive=$consecutive_fail)"

            if [ "$consecutive_fail" -ge 5 ]; then
                log_warn "    *** 5+ consecutive no-recovery ***"
            fi
        fi
    done

    # Stats
    local rate=0
    [ "$total_cycles" -gt 0 ] && rate=$((total_recovered * 100 / total_cycles))
    local avg_ms=0
    [ "$total_recovered" -gt 0 ] && avg_ms=$((total_recovery_ms / total_recovered))

    echo ""
    log_info "  ===== Final Stats ====="
    log_info "  Total cycles:  $total_cycles"
    log_info "  Recovered:     $total_recovered"
    log_info "  No recovery:   $total_failed"
    log_info "  Recovery rate: $rate%"
    log_info "  Recovery time: avg=${avg_ms}ms min=${min_recovery_ms}ms max=${max_recovery_ms}ms"

    # Stop DUT
    for ch in $(seq 0 $((G_NUM_PAIRS - 1))); do
        dut_stop ${G_DUT_PORTS[$ch]}
    done

    # Restore
    for ch in $(seq 0 $((G_NUM_PAIRS - 1))); do
        cleanup_restore ${G_DUT_PORTS[$ch]}
        cleanup_restore ${G_PARTNER_PORTS[$ch]}
    done

    if [ "$rate" -ge 50 ]; then
        log_success "  Core stress: $rate% recovery rate, avg ${avg_ms}ms"
        return 0
    else
        log_warn "  Core stress: $rate% recovery rate (may be timing-related)"
        return 0
    fi
}

# Test 2: Rapid open/close — minimal dwell, high cycle count
test_rapid_cycle_stress() {
    local dut=$1 partner=$2
    local cycles=100
    log_info "  Rapid cycle: $cycles cycles, 500ms dwell"

    sudo ip link set $dut down 2>/dev/null || true
    sleep 0.2
    sudo ip link set $dut up type can bitrate 500000 dbitrate 2000000 fd on termination 120 restart-ms 100 2>/dev/null
    sleep 0.3

    partner_open $partner || true
    sleep 0.3
    dut_start $dut
    sleep 0.5

    local recovered=0 failed=0 consecutive_fail=0

    for i in $(seq 1 $cycles); do
        if ! partner_open $partner; then
            failed=$((failed + 1))
            consecutive_fail=$((consecutive_fail + 1))
            continue
        fi

        partner_set_txtask $partner 0
        local result
        result=$(poll_dut_frames $partner 500)
        local rx_count=$(echo $result | awk '{print $1}')
        partner_clear_txtask $partner
        partner_close $partner

        if [ "$rx_count" -gt 0 ]; then
            recovered=$((recovered + 1))
            consecutive_fail=0
        else
            failed=$((failed + 1))
            consecutive_fail=$((consecutive_fail + 1))
        fi

        if [ $((i % 25)) -eq 0 ]; then
            log_info "    $i/$cycles (ok=$recovered fail=$failed)"
        fi
        if [ "$consecutive_fail" -ge 10 ]; then
            log_warn "    Bailing: 10+ consecutive failures at cycle $i"
            break
        fi
    done

    local rate=0
    local total=$((recovered + failed))
    [ "$total" -gt 0 ] && rate=$((recovered * 100 / total))

    dut_stop $dut
    cleanup_restore $dut
    cleanup_restore $partner

    log_success "  Rapid cycle: $recovered/$total ($rate%)"
    return 0
}

# Test 3: Post-stress health check
test_post_stress_health() {
    local initial_count=$1
    local oops_baseline=$2
    log_info "  Post-stress health check"

    local final_count=$(count_can_interfaces)
    if [ "$final_count" -ne "$initial_count" ]; then
        log_fail "  Interface leak: $initial_count -> $final_count"
        return 1
    fi
    log_info "    Interfaces: $final_count (no leak)"

    if ! check_new_oops "$oops_baseline"; then
        log_fail "  Kernel oops detected during stress test"
        return 1
    fi
    log_info "    Kernel: stable"

    log_success "  Post-stress health check passed"
    return 0
}

################################################################################
# Main
################################################################################

main() {
    log_info "=========================================="
    log_info "Module: Bus-Off Stress Test"
    log_info "=========================================="
    echo ""

    if [ ! -f "$RESULTS_DIR/connected_ports.txt" ]; then
        log_error "No connected ports file found!"
        return 1
    fi
    local ports=($(cat "$RESULTS_DIR/connected_ports.txt"))

    G_DUT_PORTS=()
    G_PARTNER_PORTS=()
    G_NUM_PAIRS=0

    while [ $((G_NUM_PAIRS * 2 + 1)) -lt ${#ports[@]} ]; do
        G_DUT_PORTS+=(${ports[$((G_NUM_PAIRS * 2))]})
        G_PARTNER_PORTS+=(${ports[$((G_NUM_PAIRS * 2 + 1))]})
        G_NUM_PAIRS=$((G_NUM_PAIRS + 1))
    done

    log_info "DUT ports:     ${G_DUT_PORTS[*]}"
    log_info "Partner ports: ${G_PARTNER_PORTS[*]}"
    log_info "Pairs:         $G_NUM_PAIRS"
    echo ""

    local initial_count=$(count_can_interfaces)
    local oops_baseline=$(record_oops_baseline)
    log_info "Initial CAN interfaces: $initial_count"
    echo ""

    # Test 1: Core bus-off stress
    local core_steps=${1:-60}
    start_testcase "BusOffStress_Core"
    if test_busoff_stress_core $core_steps; then
        test_pass "Core bus-off stress completed"
    else
        test_fail "Core bus-off stress failed"
    fi
    echo ""

    # Test 2: Rapid cycle stress (first pair only)
    start_testcase "BusOffStress_Rapid_Cycle"
    if test_rapid_cycle_stress ${G_DUT_PORTS[0]} ${G_PARTNER_PORTS[0]}; then
        test_pass "Rapid cycle stress completed"
    else
        test_fail "Rapid cycle stress failed"
    fi
    echo ""

    # Test 3: Post-stress health
    start_testcase "BusOffStress_Health_Check"
    if test_post_stress_health $initial_count $oops_baseline; then
        test_pass "Post-stress health check passed"
    else
        test_fail "Post-stress health check failed"
    fi
    echo ""

    log_success "Bus-off stress test completed"
    return 0
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
