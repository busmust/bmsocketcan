# 快速开始指南 / Quick Start Guide

> 快速完成 BMCAN 驱动的编译、安装和测试 / Fast track to building, installing, and testing BMCAN driver

---

## 前置要求 / Prerequisites

```bash
sudo apt install build-essential linux-headers-$(uname -r) can-utils
```

详细系统要求请查看 [README.md](../README.md#系统要求--system-requirements) / For system requirements, see [README.md](../README.md#系统要求--system-requirements)

---

## 编译安装 / Build and Install

```bash
# 1. 编译 / Build
make clean && make

# 2. 安装驱动 / Install driver
sudo make install

# 3. 加载模块 / Load module
sudo modprobe bmcan

# 4. 启动接口 / Bring up interfaces (CAN-FD mode)
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on listen-only off loopback off one-shot off termination 120
sudo ip link set can1 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
```

---

## 测试 / Test

需要物理连接 can0 和 can1 / Requires physical connection between can0 and can1:

> 提示 / Tip: `can0` 和 `can1` 只是示例，请替换为你实际物理连接的两个 CAN 通道。多通道设备在不同主机上的枚举顺序可能不同。/ `can0` and `can1` are examples; replace them with the two CAN interfaces that are physically connected in your setup. Multi-channel devices may enumerate differently on different hosts.

```bash
# 终端1 - 接收 / Terminal 1 - Receive
candump can1

# 终端2 - 发送 / Terminal 2 - Send
# 标准 CAN 2.0 帧 / Standard CAN 2.0 frame (11-bit ID, up to 8 bytes)
cansend can0 123#DEADBEEF
# 扩展 CAN 2.0 帧 / Extended CAN 2.0 frame (29-bit ID, up to 8 bytes)
cansend can0 12345678#DEADBEEF
# CAN FD 帧 / CAN FD frame (11-bit ID, up to 64 bytes)
cansend can0 123##0DEADBEEF
# CAN FD 扩展帧 / CAN FD extended frame (29-bit ID, up to 64 bytes)
cansend can0 12345678##0DEADBEEF
# CAN FD 帧（64 字节数据）/ CAN FD frame (64-byte data)
cansend can0 123##00102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F
```

---

## CAN FD 配置说明 / CAN FD Configuration

| 参数 / Parameter | 说明 / Description           | 推荐值 / Recommended |
|:----------------|:--------------------------|:--------------------|
| bitrate   | 仲裁波特率 / Arbitration bitrate    | 500000      |
| dbitrate  | 数据波特率 / Data bitrate          | 2000000      |
| sample-point / dsample-point | 采样点 / Sample point | **务必与 ECU 一致** / **Must match ECU** |
| fd        | CAN FD 模式 / CAN FD mode          | on           |
| termination| 终端电阻 / Termination resistor | 120          |

**采样点 / Sample Point**: 未指定时默认 **87.5%**（SocketCAN 内核默认值 / SocketCAN kernel default）。采样点不匹配会导致通信失败 / Mismatched sample point may cause communication failure。请务必配置为与 ECU 一致 / **Must match your ECU**：
```bash
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
```

---

## 编译产物 / Build Artifacts

- `out/bmcan.ko` - 内核驱动模块 / Kernel driver module
- `out/bmcan_api` - API 工具 / API tool for advanced features

---

## 常见问题 / Troubleshooting

### 接口未出现 / No CAN interfaces

```bash
# 检查驱动是否加载 / Check if driver is loaded
lsmod | grep bmcan

# 如果没有输出，加载驱动 / If no output, load the driver
sudo modprobe bmcan

# 检查 USB 设备 / Check USB device
lsusb | grep 0810

# 检查驱动消息 / Check driver messages
dmesg | grep bmcan
```

### cansend 成功但 candump 收不到 / Send succeeds but no receive

终端电阻未启用或物理连接问题 / Termination not enabled or physical connection issue:

```bash
# 启用终端电阻（两端都要）/ Enable termination on both ends
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
sudo ip link set can1 down
sudo ip link set can1 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120

# 验证 / Verify
ip -details link show can0 | grep termination
```

### CAN FD 帧发送失败 / CAN FD frame send error

数据长度不是合法 CAN FD DLC（有效值：0-8, 12, 16, 20, 24, 32, 48, 64 字节）或接口未启用 CAN FD / Invalid CAN FD DLC or FD mode not enabled:

```bash
# 确认 FD 模式已启用 / Confirm FD mode enabled
ip -details link show can0 | grep mtu
# 应显示 mtu 72 / Should show mtu 72
```

### 设备识别但无接口 / Device detected but no interfaces

驱动未安装或内核版本不匹配 / Driver not installed or kernel version mismatch:

```bash
sudo dmesg | tail -30

# 如果看到 "Unknown symbol" 或版本错误，重新编译安装 / If "Unknown symbol" or version error, rebuild
make clean && make
sudo make install
sudo modprobe bmcan
```

---

**下一步 / Next**: [用户手册 / User Manual](USER_MANUAL.md) | [测试指南 / Test Guide](TEST_GUIDE.md)

**最后更新 / Last Updated**: 2026-06-02
