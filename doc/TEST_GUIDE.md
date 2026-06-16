# BMCAN 测试指南 / Test Guide

> 测试程序使用方法和流程 / Testing procedures and workflows

---

## 环境验证 / Environment Validation

```bash
# 1. 检查内核模块 / Check kernel modules
lsmod | grep can
sudo modprobe can can-raw can-dev

# 2. 检查 USB 设备 / Check USB devices
lsusb | grep 0810

# 3. 加载驱动 / Load driver
sudo rmmod bmcan 2>/dev/null || true
sudo insmod out/bmcan.ko

# 4. 验证接口 / Verify interfaces
ip link show type can
dmesg | grep bmcan
```

---

## 功能测试 / Functional Tests

### 基础收发 / Basic Communication

```bash
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
sudo ip link set can1 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120

candump can1 &
cansend can0 123#DEADBEEF
```

### CAN FD 帧 / CAN FD Frame

```bash
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
cansend can0 123##00102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F
```

### txtasks - 周期发送 / Periodic Transmission

```bash
# 配置 tx 任务 / Configure tx task
sudo ./out/bmcan_api txtasks --type fixed --id 0x200 --payload DEADBEEF --cycle 100 --index 0 --device can0

# 监听接收 / Monitor reception
timeout 3 candump can1

# 清除 tx 任务 / Clear tx task
sudo ./out/bmcan_api invalidate --txtasks --tx-range 0-0 --device can0
```

### routes - 报文转发 / Message Forwarding

```bash
# 配置广播路由 / Configure broadcast route
sudo ./out/bmcan_api routes --route-type broadcast --source 0 --target 0xFF --index 1 --device can0

# 测试转发 / Test forwarding
candump can1 &
cansend can0 123#DEADBEEF

# 清除路由 / Clear route
sudo ./out/bmcan_api invalidate --route --route-range 0-1 --device can0
```

### save/load - 配置持久化 / Configuration Persistence

**仅 Gen2.5 和 Gen3 设备 / Gen2.5 and Gen3 devices only**

```bash
# 配置 tx 任务 / Configure tx task
sudo ./out/bmcan_api txtasks --type fixed --id 0x100 --payload AABBCCDD --cycle 100 --index 0 --device can0

# 保存到设备存储 / Save to device storage
sudo ./out/bmcan_api save --device can0

# 修改配置（不同 ID）/ Modify configuration (different ID)
sudo ./out/bmcan_api txtasks --type fixed --id 0x200 --payload CHANGED --cycle 100 --index 0 --device can0

# 从设备存储加载（恢复 ID=0x100）/ Load from device storage (restores ID=0x100)
sudo ./out/bmcan_api load --device can0

# 验证恢复的配置 / Verify restored configuration
timeout 3 sh -c "candump can1 | grep 100"

# 清除设备存储 / Clear device storage
sudo ./out/bmcan_api clear --device can0
```

### logging - 流量日志 / Traffic Logging

**仅 Gen2.5 设备 / Gen2.5 devices only** (X2R, X4R)

```bash
# 开始记录 / Start logging
sudo ./out/bmcan_api logging --mode always --format bbd --channels 0xFFFF --device can0

# 生成流量 / Generate traffic
for i in {1..100}; do cansend can0 123#DEADBEEF; sleep 0.1; done

# 停止记录 / Stop logging
sudo ./out/bmcan_api logging --mode off --device can0
```

### replay - 报文回放 / Message Replay

**仅 Gen2.5 设备 / Gen2.5 devices only**

```bash
# 开始回放 / Start replay
sudo ./out/bmcan_api replay --mode always --format bbd --channels 0xFFFF --device can0

# 停止回放 / Stop replay
sudo ./out/bmcan_api replay --mode off --device can0
```

---

## 测试矩阵 / Test Matrix

| 功能 / Feature      | 测试方法 / Test Method                        | 预期结果 / Expected Result               |
|:--------------------|:-----------------------------------------------|:---------------------------------------|
| 基础 TX/RX / Basic TX/RX  | cansend/candump                          | 远程端口接收帧 / Frames received on remote port   |
| CAN FD             | 64 字节帧 / 64-byte frame                     | FD 帧发送成功 / FD frames transmitted       |
| txtasks            | --type fixed                               | 接收到周期帧 / Periodic frames received      |
| routes             | --route-type broadcast                      | 帧已转发 / Frames forwarded             |
| save/load          | save/load                                | 配置已恢复 / Configuration restored        |
| logging            | --mode always (仅 Gen2.5 / Gen2.5 only)       | 流量记录到设备 / Traffic recorded to device     |
| replay             | --mode always (仅 Gen2.5 / Gen2.5 only)       | 从设备回放帧 / Frames replayed from device   |

---

## 自动化测试套件 / Automated Test Suite

### 硬件要求 / Hardware Requirements

推荐使用两台 4 通道 Gen3 设备（如 X4 / F043），通过 CAN 总线交叉连接 / Recommended: two 4-channel Gen3 devices (e.g. X4 / F043), cross-connected via CAN bus:

```
Device A          Device B
can0 ─────────── can6
can1 ─────────── can7
can2 ─────────── can4
can3 ─────────── can5
```


### 运行测试 / Run Tests

```bash
sudo bash test/run_all_tests.sh
```

测试模块 / Test modules:
- `modules/bm_mode_test.sh` - 模式切换 / Mode switching
- `modules/bm_basic_communication.sh` - 基础 TX/RX / Basic TX/RX
- `modules/bm_comprehensive_communication.sh` - 所有帧类型 / All frame types
- `modules/bm_tx_task.sh` - 周期发送 / Periodic transmission
- `modules/bm_route.sh` - 报文路由 / Message routing
- `modules/bm_filter.sh` - ID 过滤 / ID filtering
- `modules/bm_config_persistence.sh` - 配置保存/加载 / Config save/load
- `modules/bm_stress_test.sh` - 长时间稳定性 / Long-running stability

结果保存到 / Results saved to: `test/results/test_results.xml`

---

## 性能测试 / Performance Testing

```bash
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120

# 生成流量 / Generate traffic
cangen can0 -v -g 1 &
CANGEN_PID=$!

# 等待并检查统计 / Wait and check statistics
sleep 10
ip -details -statistics link show can0 | grep -A1 'TX:'

# 停止生成器 / Stop generator
kill $CANGEN_PID
```

---

## 故障排查 / Troubleshooting

常见问题请参考 [快速开始指南 - 常见问题](QUICK_START.md#常见问题--troubleshooting) / For common issues, see [Quick Start - Troubleshooting](QUICK_START.md#常见问题--troubleshooting)

---

## 注意事项 / Important Notes

详见 [README.md - 注意事项](../README.md#注意事项--important-notes) / See [README.md - Important Notes](../README.md#注意事项--important-notes)
