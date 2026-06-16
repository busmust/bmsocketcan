#!/bin/bash
################################################################################
# BMCAN Test Framework - Shared Constants and Logging
#
# Source this file in all scripts that need colors, logging, or path setup.
# Paths are derived from common.sh's own location (lib/common.sh -> test/),
# so they work regardless of which script sources this file.
#
# Usage:
#   SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#   source "$SCRIPT_DIR/../lib/common.sh"   # from modules/
#   source "$SCRIPT_DIR/lib/common.sh"       # from test root
################################################################################

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

################################################################################
# Path Setup - derived from common.sh location (always correct)
################################################################################

# bm_test root = one directory up from lib/ (where this file lives)
_BM_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Allow environment overrides (for run_all_tests.sh which sets these first)
LIB_DIR="${LIB_DIR:-$_BM_TEST_ROOT/lib}"
MODULES_DIR="${MODULES_DIR:-$_BM_TEST_ROOT/modules}"
RESULTS_DIR="${RESULTS_DIR:-$_BM_TEST_ROOT/results}"

# API tool path - binary lives in project out/ directory
API_BIN="${API_BIN:-$_BM_TEST_ROOT/../out/bmcan_api}"

# Driver module path - override with DRIVER_PATH for custom location
DRIVER_PATH="${DRIVER_PATH:-$_BM_TEST_ROOT/../out/bmcan.ko}"

# Connected ports file
CONNECTED_PORTS_FILE="$RESULTS_DIR/connected_ports.txt"

# JUnit report output
JUNIT_REPORT="$RESULTS_DIR/test_results.xml"

# Hardware config - always in bm_test root
HW_CONF_BASH="$_BM_TEST_ROOT/hardware_config.conf"

# Config backup directory
BACKUP_DIR="/tmp/bmcan_config_backup"

################################################################################
# Logging Functions
################################################################################

log_header() {
    echo ""
    echo "=========================================="
    echo -e "${BLUE}$1${NC}"
    echo "=========================================="
}

log_info() {
    echo "[INFO] $*"
}

log_error() {
    echo -e "${RED}[ERROR] $*${NC}"
}

log_success() {
    echo -e "${GREEN}[\xE2\x9C\x93] $*${NC}"
}

log_fail() {
    echo -e "${RED}[\xE2\x9C\x97] $*${NC}"
}

log_warn() {
    echo -e "${YELLOW}[WARN] $*${NC}"
}

################################################################################
# Device Generation Detection
################################################################################

# Detect device generation from currently connected USB devices (lsusb)
# Returns: "gen3" | "gen25" | "gen2" | "unknown"
detect_generation() {
    local usb_ids=$(lsusb 2>/dev/null | grep -i '0810:' | sed 's/.*ID 0810://' | sed 's/ .*//' | tr '[:upper:]' '[:lower:]')
    for id in $usb_ids; do
        case "$id" in
            f013|f023|f043|0043|0083) echo "gen3"; return ;;
            e122|e142) echo "gen25"; return ;;
            f012|f112|f122|f142) echo "gen2"; return ;;
        esac
    done
    echo "unknown"
}

# Convenience wrappers
is_gen25_device() { [ "$(detect_generation)" = "gen25" ]; }
is_gen3_device() { [ "$(detect_generation)" = "gen3" ]; }

################################################################################
# Kernel Oops Baseline
# ARM boards may have boot-time "Call trace" entries in dmesg that are NOT
# from our driver. Record baseline count at test start and only check new ones.
################################################################################

# Record current oops count as baseline
record_oops_baseline() {
    local count
    count=$(sudo dmesg | grep -cE 'Oops|BUG:|panic|Call trace' 2>/dev/null || true)
    count=$(echo "$count" | tr -d '[:space:]')
    count=${count:-0}
    echo "$count"
}

# Check for NEW oops since baseline (arg $1 = baseline count)
check_new_oops() {
    local baseline=${1:-0}
    local current
    current=$(sudo dmesg | grep -cE 'Oops|BUG:|panic|Call trace' 2>/dev/null || true)
    current=$(echo "$current" | tr -d '[:space:]')
    current=${current:-0}
    local new_oops=$((current - baseline))
    if [ "$new_oops" -gt 0 ]; then
        log_error "New kernel oops detected: $new_oops (baseline=$baseline, current=$current)"
        return 1
    fi
    return 0
}
