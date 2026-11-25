# BMCAN Test Framework

> Automated tests for the BMCAN USB-CAN FD driver

---

## Quick Start

```bash
cd test

# Run all tests
bash run_all_tests.sh

# Verify hardware connections
bash tools/verify_hardware.sh

# Update hardware config (auto-discover)
bash tools/update_hardware_config.sh --auto
```

---

## Directory Structure

```
test/
├── run_all_tests.sh          # Main entry point
├── hardware_config.conf      # Hardware connection mapping (bash source)
├── hardware_config.json      # Hardware connection mapping (JSON, auto-generated)
│
├── lib/                      # Shared libraries
│   ├── common.sh             # Colors, logging, path constants
│   ├── config_manager.sh     # Port config backup/restore/clear
│   ├── hardware_helper.sh    # Hardware query and port initialization
│   └── junit_xml.sh          # JUnit XML report generator
│
├── modules/                  # Test modules (sourced by run_all_tests.sh)
│   ├── bm_mode_test.sh
│   ├── bm_basic_communication.sh
│   ├── bm_comprehensive_communication.sh
│   ├── bm_route.sh
│   ├── bm_tx_task.sh
│   ├── bm_filter.sh
│   ├── bm_config_persistence.sh
│   └── bm_stress_test.sh
│
├── tools/                    # Standalone utilities
│   ├── verify_hardware.sh    # Verify all port connections
│   └── update_hardware_config.sh  # Regenerate hardware_config.*
│
└── results/                  # Test output (gitignored)
    ├── test_results.xml      # JUnit report
    └── connected_ports.txt   # Active port list
```

---

## Test Phases

| Phase | Modules | Description |
|-------|---------|-------------|
| 0 | Port init | Initialize hardware config port pairs |
| 1 | `bm_mode_test` | Work mode configuration |
| 2 | `bm_basic_communication`, `bm_comprehensive_communication`, `bm_route`, `bm_tx_task`, `bm_filter` | CAN communication |
| 3 | `bm_config_persistence`, `bm_stress_test` | Persistence and stress |

---

## Hardware Config

Example connections for two four-channel devices. Set the port pairs to match your wiring before running the tests.

```
can0 <-> can6     can1 <-> can7
can2 <-> can4     can3 <-> can5
```

Update configuration:
```bash
bash tools/update_hardware_config.sh --auto            # Auto-discover
bash tools/update_hardware_config.sh --set can0:can6   # Manual
```

Use in scripts:
```bash
source lib/common.sh
source lib/hardware_helper.sh
hw_init
hw_get_primary_pair TX RX
hw_init_pair "$TX" "$RX"
```

---

## Running the tests in CI

```bash
cd /path/to/bmsocketcan
sudo rmmod bmcan 2>/dev/null || true
sudo insmod out/bmcan.ko
sudo bash test/run_all_tests.sh
```

JUnit parser config:
- Directory: `test/results`
- Pattern: `test_results.xml`
