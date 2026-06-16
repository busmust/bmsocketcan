# BMCAN 用户手册 / User Manual

> API 工具完整说明 / Complete API tool reference and usage guide

---

## 工具语法 / Tool Syntax

```bash
./out/bmcan_api <command> [options] --device <interface>
```

**通用选项 / Common Options**:
- `--apply` - 设置后保存配置到设备存储（仅 Gen2.5+/Gen3 支持）/ Save config to device storage after setting (Gen2.5+/Gen3 only)
- `--device <interface>` - 指定 CAN 接口（如 can0）/ Target CAN interface (e.g. can0)

---

## txtasks - 周期发送 / Periodic Transmission

配置自动周期 CAN 帧发送 / Configure automatic periodic CAN frame transmission.

| 参数 / Parameter  | 说明 / Description              | 值 / Values                           |
|:----------------|:-----------------------------|:--------------------------------------|
| `--type`   | 任务类型 / Task type               | fixed, incdata, incid, randomdata, randomid|
| `--id`     | CAN ID / CAN ID                  | 十六进制 / Hexadecimal                        |
| `--payload`| 数据内容 / Data content            | 十六进制 / Hexadecimal                        |
| `--cycle`  | 发送周期 / Transmission period     | 毫秒 / Milliseconds                        |
| `--index`  | 任务索引 / Task index (0-63)       | 0-63                              |

```bash
# 固定数据发送 / Fixed data transmission
sudo ./out/bmcan_api txtasks --type fixed --id 0x123 --payload DEADBEEF --cycle 100 --index 0 --device can0

# CAN FD 帧 / CAN FD frame
sudo ./out/bmcan_api txtasks --type fixed --id 0x123 --payload 0102030405060708 --cycle 100 --index 0 --device can0

# 递增数据 / Incremental data
sudo ./out/bmcan_api txtasks --type incdata --id 0x200 --payload 00000000 --cycle 50 --index 1 --device can0

# 递增 ID / Incremental ID
sudo ./out/bmcan_api txtasks --type incid --id 0x100 --payload 12345678 --cycle 100 --index 2 --device can0
```

---

## routes - 报文路由 / Message Routing

配置通道间自动消息转发 / Configure automatic message forwarding between channels.

| 参数 / Parameter    | 说明 / Description          | 值 / Values           |
|:------------------|:-----------------------------|:---------------------|
| `--route-type`| 路由类型 / Route type         | broadcast, unicast, idmap, flagsmap, idflagsmap |
| `--source`    | 源通道 / Source channel      | 0-15             |
| `--target`    | 目标通道 / Target channel(s)   | 0-15, 0xFF (广播/broadcast)|
| `--id-value`  | CAN ID 过滤值 / CAN ID filter value  | 十六进制 / Hexadecimal       |
| `--id-mask`   | ID 掩码 / ID mask            | 十六进制 / Hexadecimal       |
| `--index`     | 规则索引 / Rule index (0-63)  | 0-63             |

```bash
# 广播路由（转发所有帧）/ Broadcast routing (forward all frames)
sudo ./out/bmcan_api routes --route-type broadcast --source 0 --target 0xFF --index 1 --device can0

# 单播路由（转发指定 ID）/ Unicast routing (forward specific ID)
sudo ./out/bmcan_api routes --route-type unicast --source 0 --target 1 --id-value 0x123 --id-mask 0x7FF --index 0 --device can0

# ID 范围过滤 / ID range filtering
sudo ./out/bmcan_api routes --route-type unicast --source 0 --target 1 --id-value 0x100 --id-mask 0x700 --index 0 --device can0
```

---

## invalidate - 清除运行时配置 / Clear Runtime Configuration

清除固件当前运行时的 txtasks/routes 配置。不影响设备存储中已保存的配置。
Clear active runtime config. Saved config in device storage is not affected.

| 参数 / Parameter     | 说明 / Description           | 值 / Values         |
|:--------------------|:--------------------------------|:-------------------|
| `--txtasks`    | 清除 tx 任务 / Clear tx tasks       | (标志/flag)         |
| `--tx-range`   | 任务索引范围 / Task index range     | 0-63           |
| `--route`      | 清除路由 / Clear routes        | (标志/flag)         |
| `--route-range`| 路由索引范围 / Route index range   | 0-63           |
| `--all`       | 清除全部配置 / Clear all config    | (标志/flag)         |

```bash
# 清除单个 tx 任务 / Clear single tx task
sudo ./out/bmcan_api invalidate --txtasks --tx-range 0-0 --device can0

# 清除所有 tx 任务 / Clear all tx tasks
sudo ./out/bmcan_api invalidate --txtasks --tx-range 0-63 --device can0

# 清除路由 / Clear routes
sudo ./out/bmcan_api invalidate --route --route-range 0-63 --device can0

# 清除全部（txtasks + routes + logging + replay）/ Clear all
sudo ./out/bmcan_api invalidate --all --device can0
```

---

## save - 保存配置 / Save Configuration

保存当前配置到设备非易失性存储 / Save current config to device storage.

**仅 Gen2.5+ 设备 / Gen2.5+ devices only** (Gen2.5: X2R, X4R; Gen3: X1, X2, X4, XL2, XL4)

```bash
sudo ./out/bmcan_api save --device can0
```

保存范围 / Saved config includes:
- txtask entries (0-63)
- route entries (0-63)
- Config mask: txtask (bit 9) + route (bit 10)

---

## load - 加载配置 / Load Configuration

从设备存储加载已保存的配置 / Load saved config from device storage.

**仅 Gen2.5+ 设备 / Gen2.5+ devices only**

```bash
sudo ./out/bmcan_api load --device can0
```

---

## clear - 清除已保存配置 / Clear Saved Configuration

清除设备存储中的配置。`invalidate` 清运行时，`clear` 清存储，两者互不影响。
Clear saved config from device storage. `invalidate` clears runtime, `clear` clears storage.

**仅 Gen2.5+ 设备 / Gen2.5+ devices only**

```bash
sudo ./out/bmcan_api clear --device can0
```

---

## logging - 设备日志 / On-Device Logging

**仅 Gen2.5 设备 / Gen2.5 devices only** (X2R, X4R)

记录 CAN 流量到设备存储 / Record CAN traffic to device storage.

| 参数 / Parameter    | 说明 / Description            | 值 / Values                    |
|:------------------|:-------------------------------|:------------------------------|
| `--mode`      | 工作模式 / Working mode            | always, filter, trigger, off|
| `--format`    | 文件格式 / File format            | bbd, csv                  |
| `--channels`  | 通道掩码 / Channel mask           | 0xFFFF (所有通道/all channels)     |
| `--direction` | 记录方向 / Recording direction     | rx, tx, all              |

```bash
# 记录所有流量 / Record all traffic
sudo ./out/bmcan_api logging --mode always --format bbd --channels 0xFFFF --device can0

# 按 ID 过滤 / Filter by ID
sudo ./out/bmcan_api logging --mode filter --format bbd --id 0x123 --mask 0x7FF --device can0

# 停止记录 / Stop logging
sudo ./out/bmcan_api logging --mode off --device can0
```

---

## replay - 报文回放 / Message Replay

**仅 Gen2.5 设备 / Gen2.5 devices only** (X2R, X4R)

从设备存储回放已记录的 CAN 流量 / Replay recorded CAN traffic from device storage.

| 参数 / Parameter    | 说明 / Description      | 值 / Values                |
|:------------------|:---------------------|:----------------------|
| `--mode`      | 工作模式 / Working mode  | always, filter, off   |
| `--format`    | 文件格式 / File format  | bbd, csv              |
| `--channels`  | 通道掩码 / Channel mask | 0xFFFF (所有通道/all channels)|
| `--cyclic`    | 循环回放 / Loop replay  | 0 or 1                |

```bash
# 回放所有记录流量 / Replay all recorded traffic
sudo ./out/bmcan_api replay --mode always --format bbd --channels 0xFFFF --device can0

# 循环回放 / Cyclic replay
sudo ./out/bmcan_api replay --mode always --format bbd --channels 0xFFFF --cyclic 1 --device can0

# 停止回放 / Stop replay
sudo ./out/bmcan_api replay --mode off --device can0
```

---

## sysfs 接口 / sysfs Interface

> 高级 API：驱动为每个 CAN 接口在 `/sys/class/net/canX/` 下提供 sysfs 文件，
> 使用 BMAPI 公开结构体的十六进制编码 (hex blob) 作为稳定 ABI。
> 推荐使用 `bmcan_api` 工具；熟悉 BMAPI 结构体的用户可直接操作 sysfs。
>
> 写入需 root 权限，推荐使用 `echo ... | sudo tee` 方式。

### ABI 映射 / ABI Mapping

| sysfs 文件 | 读/写 | 结构体 (`bm_usb_def.h`)              | 大小    | 说明                |
|:-----------|:-----|:--------------------------------------|:--------|:--------------------|
| `txtasks`  | R/W  | `BM_TxTaskTypeDef`                   | 128 B   | 周期发送任务        |
| `routes`   | R/W  | `BM_MessageRouteTypeDef`             | 16 B    | 路由转发规则        |
| `logging`  | R/W  | `BM_LoggingConfigTypeDef`            | 96 B    | 离线录制 (Gen2.5)   |
| `replay`   | R/W  | `BM_ReplayConfigTypeDef`             | 88 B    | 离线回放 (Gen2.5)   |
| `save`     | W    | config mask (hex)                    | —       | 保存到设备存储      |
| `load`     | W    | config mask (hex)                    | —       | 从设备加载配置      |
| `clear`    | W    | config mask (hex)                    | —       | 清除设备存储配置    |

**ABI 规则 / ABI Rules**:
- 字节序：所有字段 little-endian (wire format)
- 结构体布局与 `bm_usb_def.h` 中的 `#pragma pack(1)` 定义完全一致，跨固件版本稳定
- hex blob 不足结构体大小时自动补零，超出时返回写入错误
- `txtasks` / `routes` 读取只返回前 4 条缓存 (index 0-3)，写入支持 index 0-63

**Config Mask 位定义 / Config Mask Bits**:

| 位 / Bit | 配置 / Config   | 位 / Bit | 配置 / Config    |
|:--------|:---------------|:--------|:----------------|
| 0        | Mode           | 8        | RX Filter       |
| 2        | Bitrate        | 9        | Txtask          |
| 3        | Termination    | 10       | Route           |
|          |                | 11       | Logging         |
|          |                | 12       | Replay          |

### 操作示例 / Usage Examples

**txtasks** — `index hex_data` (hex 长度 = 256 字符 = 128 bytes):
```bash
# 写入 index 0，固定帧 CAN ID 0x123，payload DEADBEEF，周期 100ms
echo "0 0101000000000000640000010000000001230000000000000000000000000000DEADBEEF0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" | sudo tee /sys/class/net/can0/txtasks >/dev/null

# 读取 (返回 index 0-3 缓存)
sudo cat /sys/class/net/can0/txtasks
```

**routes** — `index hex_data` (hex 长度 = 32 字符 = 16 bytes):
```bash
# 广播路由: source=0, target=0xFF
echo "0 010001000100000000000000FF000000" | sudo tee /sys/class/net/can0/routes >/dev/null

# 单播路由: source=0, target=1, ID 过滤 0x123/0x7FF
echo "1 01000100000000000123000000000000" | sudo tee /sys/class/net/can0/routes >/dev/null

# 读取
sudo cat /sys/class/net/can0/routes
```

**logging** — hex blob (96 bytes = 192 hex chars):
```bash
# always 模式, bbd 格式, 所有通道
echo "01010000FFFF010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" | sudo tee /sys/class/net/can0/logging >/dev/null

# 停止录制
echo "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" | sudo tee /sys/class/net/can0/logging >/dev/null

# 读取当前配置
sudo cat /sys/class/net/can0/logging
```

**replay** — hex blob (88 bytes = 176 hex chars):
```bash
# always 模式, bbd 格式, 所有通道, 非循环
echo "01010000FFFF00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" | sudo tee /sys/class/net/can0/replay >/dev/null

# 停止回放
echo "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" | sudo tee /sys/class/net/can0/replay >/dev/null
```

**save / load / clear** — config mask (hex):
```bash
echo "0x0200" | sudo tee /sys/class/net/can0/save >/dev/null   # txtask
echo "0x0400" | sudo tee /sys/class/net/can0/save >/dev/null   # route
echo "0xFFFF" | sudo tee /sys/class/net/can0/clear >/dev/null  # all config
echo "0xFFFF" | sudo tee /sys/class/net/can0/load >/dev/null   # load all
```

### 错误码 / Error Codes

sysfs 写入失败时，驱动返回 `< 0` 的错误码：

| 错误 / Error     | 含义 / Meaning                              |
|:-----------------|:--------------------------------------------|
| `-EINVAL`        | hex 格式错误、index 越界、blob 长度不匹配    |
| `-ENODEV`        | 设备已断开                                   |
| `-EIO`           | USB 控制传输失败 (设备通信错误)               |
| `-ENOMEM`        | 内核内存不足                                 |
| `-ENOTSUPP`      | 设备不支持此功能 (如 Gen3 写入 logging/replay)|
| `-EPERM`         | 非特权用户写入 (需 root)                     |

写入失败可通过 `dmesg | tail` 查看详细原因。

---

## 终端电阻 / Termination Resistor

驱动默认启用 120Ω 终端电阻。两端设备均需开启，否则信号反射导致通信错误。
120Ω termination is enabled by default. Both ends of the bus must enable it to avoid signal reflections.

```bash
sudo ip link set can0 type can termination 0    # 关闭 / Disable
sudo ip link set can0 type can termination 120   # 开启 / Enable
```

---

## 设备支持 / Device Support

详见 [README.md - 支持的设备](../README.md#支持的设备--supported-devices) / See [README.md - Supported Devices](../README.md#支持的设备--supported-devices)

---

## 注意事项 / Important Notes

详见 [README.md - 注意事项](../README.md#注意事项--important-notes) / See [README.md - Important Notes](../README.md#注意事项--important-notes)
