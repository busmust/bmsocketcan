# BMCAN USB-CAN FD Driver

> BUSMUST BMCAN USB-CAN FD 设备 Linux 内核驱动 / Linux kernel driver for BUSMUST USB-CAN FD devices

---

## 主要功能 / Features

- **SocketCAN 兼容 / SocketCAN Compatible** - 标准 Linux CAN 接口 / Standard Linux CAN interface
- **CAN FD 支持 / CAN FD Support** - 0-64 字节 / bytes, 比特率切换 / bitrate switching
- **多通道 / Multi-Channel** - 最多 8 通道 / Up to 8 channels, 独立配置 / independent configuration
- **终端电阻 / Software Termination** - 软件控制 120Ω / 120Ω control via ip-link
- **高级功能 / Advanced Features** - txtasks, routes, logging, replay
- **配置持久化 / Config Persistence** - 保存/加载到设备存储 / Save/load to device storage

---

## 系统要求 / System Requirements

- **内核 / Kernel**: >= 4.19
- **内核选项 / Kernel Options**: `CONFIG_CAN=y`, `CONFIG_CAN_RAW=y`, `CONFIG_CAN_DEV=y`
- **内核模块 / Kernel Modules**: can, can-raw, can-dev
- **依赖 / Dependencies**: build-essential, linux-headers, can-utils

---

## 支持的设备 / Supported Devices

| 代际 / Generation | 产品 / Product | 产品ID / Product IDs | 通道 / Channels | 存储 / Storage | Logging/Replay |
|:-----------------|:---------------|:---------------------|:----------------|:--------------|:---------------|
| Gen2             | X1             | F012                 | 1               | 无 / No        | 无 / No         |
| Gen2             | X1 Pro         | F112                 | 1               | 无 / No        | 无 / No         |
| Gen2             | X2             | F122                 | 2               | 无 / No        | 无 / No         |
| Gen2             | X4             | F142                 | 4               | 无 / No        | 无 / No         |
| Gen2.5           | X2R            | E122                 | 2               | 支持 / Yes     | 支持 / Yes      |
| Gen2.5           | X4R            | E142                 | 4               | 支持 / Yes     | 支持 / Yes      |
| Gen3             | X1             | F013                 | 1               | 支持 / Yes     | 无 / No         |
| Gen3             | X2             | F023                 | 2               | 支持 / Yes     | 无 / No         |
| Gen3             | X4             | F043                 | 4               | 支持 / Yes     | 无 / No         |
| Gen3             | XL2            | 0043                 | 2               | 支持 / Yes     | 无 / No         |
| Gen3             | XL4            | 0083                 | 4               | 支持 / Yes     | 无 / No         |

**功能说明 / Feature Notes**:
- txtasks / routes: 所有代际 / All generations
- logging / replay: 仅 Gen2.5 / Gen2.5 only (X2R, X4R)
- save / load / clear: Gen2.5 和 Gen3 / Gen2.5 and Gen3

**终端电阻 / Termination**: 驱动默认启用 120Ω 终端电阻。如需调整请参考 [用户手册 / User Manual](doc/USER_MANUAL.md#终端电阻--termination-resistor)。/ The driver enables 120Ω termination by default. See [User Manual](doc/USER_MANUAL.md#终端电阻--termination-resistor) for details.

---

## 快速开始 / Quick Start

```bash
# 1. 安装依赖 / Install dependencies
sudo apt install build-essential linux-headers-$(uname -r) can-utils

# 2. 编译 / Build
make clean && make

# 3. 安装驱动 / Install driver (auto-loads on device plug-in)
sudo make install

# 4. 启动接口 / Bring up interface (CAN-FD mode)
sudo ip link set can0 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
sudo ip link set can1 up type can bitrate 500000 sample-point 0.75 dbitrate 2000000 dsample-point 0.80 fd on termination 120
```

**测试 / Test** (需要物理连接 can0 和 can1 / requires physical connection between can0 and can1)

> 提示 / Tip: `can0` 和 `can1` 只是示例，请替换为你实际物理连接的两个 CAN 通道。/ `can0` and `can1` are examples; replace them with the two CAN interfaces that are physically connected in your setup.
```bash
# 终端1 - 接收 / Terminal 1 - Receive
candump can1

# 终端2 - 发送 / Terminal 2 - Send
cansend can0 123#DEADBEEF
```

详细步骤和常见问题 / For detailed steps and troubleshooting: [快速开始指南 / Quick Start Guide](doc/QUICK_START.md)

---

## 文档导航 / Documentation

| 文档 / Document         | 说明 / Description                              |
|:------------------------|:---------------------------------------------|
| [快速开始 / Quick Start](doc/QUICK_START.md)     | 快速上手和常见问题 / Getting started and troubleshooting |
| [用户手册 / User Manual](doc/USER_MANUAL.md)    | API 工具完整说明 / Complete API tool reference    |
| [测试指南 / Test Guide](doc/TEST_GUIDE.md)      | 测试流程和自动化 / Testing procedures and automation |

---

## API 工具 / API Tool

驱动包含 `bmcan_api` 工具用于高级功能 / Driver includes `bmcan_api` tool for advanced features:

| 命令 / Command    | 说明 / Description                        |
|:------------------|:---------------------------------------|
| `txtasks`         | 周期发送 / Periodic transmission (tx tasks)     |
| `routes`          | 路由转发 / Message routing                    |
| `logging`         | 设备日志 / On-device logging (Gen2.5 only)    |
| `replay`         | 报文回放 / Message replay (Gen2.5 only)       |
| `save`            | 保存配置 / Save config to device storage        |
| `load`            | 加载配置 / Load config from device storage      |
| `invalidate`       | 清除运行时配置（不影响已保存的配置）/ Clear runtime config (keeps saved config) |

详见 / See [用户手册 / User Manual](doc/USER_MANUAL.md)

---

## 注意事项 / Important Notes

**采样点 / Sample Point**: 默认 87.5%，**务必配置为与 ECU 一致**。详见 [快速开始指南 / Quick Start Guide](doc/QUICK_START.md#can-fd-配置说明--can-fd-configuration)

**工作模式 / Work Modes**: 切换回普通模式时建议显式清除可能残留的控制模式，例如 `listen-only off loopback off one-shot off`。/ When switching back to normal mode, explicitly clear sticky ctrlmodes such as `listen-only off loopback off one-shot off`.

**高级功能 / Advanced Features**:
- txtasks, routes, logging, replay 等功能为 BMCAN 独有特性 / txtasks, routes, logging, replay are BMCAN exclusive features
- 当前测试阶段仍在完善中，使用时请谨慎操作 / Current testing phase is under development, use with caution
- 遇到问题请及时反馈 / Report issues promptly:
  - 官方网站 / Official website: https://www.busmust.com
  - 技术支持 / Technical support: support@busmust.com

**维护者 / Maintainer**: BUSMUST 技术支持团队 / BUSMUST Technical Support Team
**许可证 / License**: GPL-2.0-or-later (见 / See [LICENSE](LICENSE))
**最后更新 / Last Updated**: 2026-06-02
