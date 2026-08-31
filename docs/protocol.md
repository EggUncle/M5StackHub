# USB / BLE 通信协议

[返回首页](../README.md)

本页依据 [src/main.cpp](../src/main.cpp) 的 `handleCommand`、`applyBleSnapshot`、
`queueBleSnapshot` 与 BLE 客户端实现。协议标识为 `codex-pet/1`，为兼容保留历史名称。
设备不直接访问 Codex、天气 API 或 Mac 系统统计，也不应接收账户 Token。

## USB 串口

波特率 115200。UTF-8/ASCII 文本行，以 `\n` 结束，忽略 `\r`，每行上限 180 字节。
建议字段与模型名使用 ASCII；多数命令有 `OK` 或 `ERR` 响应。
参数应发送合法数字，不依赖宽松转换；该协议不是严格的类型校验 API。

| 命令 | 参数与行为 |
| --- | --- |
| `hello` / `ping` | 回复 `PONG codex-pet/1 pet=stopwatch` |
| `status` | 输出 `STATUS ...` 和 `WEATHER ...`，不主动刷新数据源 |
| `wake` | 唤醒显示、记录活动 |
| `sleep` / `tuck` | 关闭显示，不主动切断已连接的 BLE |
| `state idle` | 状态为 `idle`、`running`、`needs_input`、`ready`、`blocked` 之一 |
| `tasks N` | 任务数限制在 0–99；当前表盘不单独展示任务数 |
| `quota WEEK FIVE_HOUR` | 两个剩余百分比都必须提供；负值转换为未知 -1，其他值限制在 0–100 |
| `system CPU MEM` | 两个已用百分比都必须提供；负值为未知 -1，其他值限制在 0–100 |
| `model TEXT` | 保存最多 24 字节；显示转大写并截至前 15 字节；非字库字符可能空白 |
| `clock EPOCH_UTC TZ_SECONDS` | 秒级 UTC 时间戳，至少 1600000000；时区偏移限制在 ±18 小时；同步可用 RTC |
| `weather TEMP WMO IS_DAY UPDATED_EPOCH` | 整数摄氏度 -80–60，天气码 0–99，非零为白天，最后为秒级 UTC 更新时间 |
| `reduced_motion 1` / `reduced_motion 0` | 开启 / 关闭减少动态效果；开启后禁用工作波前，不再启动新的唤醒渐入 |

状态只在改变时作为新活动记录；相同状态重复下发用于心跳，不强行反复点亮屏幕。
配额、模型、天气、任务数、状态、显示开关与减少动态设置会按实现写入 NVS；CPU、内存不持久化。
避免在测试时高频修改会持久化的字段，测试值可能在重启后保留。

`wave`、`jump`、`run_left`、`run_right` 为旧协议兼容入口，不会在当前表盘画出宠物。
`servo_home` 在 StopWatch 回复 `ERR servo unavailable`。

### 人工测试示例

以下均为**示例数据**，发送后会改变设备显示，并可能被正常 BLE 快照覆盖。
配额 86 不是默认值、真实账户值，也不是应长期写死的配置。

```text
wake
model GPT-5.6-SOL
quota 86 72
system 12 59
state running
status
```

时钟应使用当前 UTC 时间戳；不要把示例固定时间当作实时同步。可在 Mac 终端生成待发送文本：

```sh
date -u '+clock %s 28800'
```

该命令只打印文本，不会发送到设备。天气更新时间也应使用真实数据的时间戳。

## BLE 角色和 UUID

**StopWatch 是 GATT 客户端，Mac 桥接程序是广播服务的外设端。**
设备匹配服务 UUID，或旧广播名 `StackChanCodex`；连接后仍需要下面的服务和特征。

| 项目 | UUID / 内容 |
| --- | --- |
| 服务 | `e7d6d101-5a2d-4b7a-9c0e-123456789abc` |
| 快照特征（读取 / 通知） | `e7d6d102-5a2d-4b7a-9c0e-123456789abc` |
| 动作特征（设备向主机写入） | `e7d6d109-5a2d-4b7a-9c0e-123456789abc` |
| 触摸激活动作 | ASCII `activate` |

协议没有提供已验证的安全认证承诺。不要把广播名称视为身份验证，也不要通过它传输凭据。
如果附近同时有多个同协议主机，请在主机侧控制广播，避免连接到非预期来源。

## 快照格式

快照是换行分隔的 `KEY=VALUE` 文本。键名区分大小写；`PROTOCOL` 必须匹配，其他字段可按规则省略。
下面是完整格式示例，时间和所有指标均为演示值：

```text
PROTOCOL=codex-pet/1
STATE=running
TASKS=1
WEEK=86
FIVE_HOUR=72
MODEL=GPT-5.6-SOL
E=1788056460
TZ=28800
WT=29
WC=3
WD=1
WU=1788056400
CPU=12
MEM=59
```

| 字段组 | 更新要求 |
| --- | --- |
| `STATE`、`TASKS`、`MODEL` | 可分别更新 |
| `WEEK` + `FIVE_HOUR` | 必须同时非空，才更新配额 |
| `CPU` + `MEM` | 必须同时非空，才更新系统指标 |
| `WT` + `WC` + `WD` + `WU` | 必须四项同时非空，才更新天气 |
| `E` + `TZ` | `E` 有效才校时；省略 `TZ` 会按 0 偏移处理，所以 UTC+8 主机应每次带上 28800 |

只发 `WEEK` 会被忽略，这是排查“配额一直不变”时首先需要检查的地方。
`WEEK` 是周配额剩余，不是已用；若数据源返回已用比例，由主机先完成转换。
快照包含字段不等于字段已被正确应用：检查 `OK ...` 日志与 `status` 返回值。

### 通知分片

单条完整快照可以直接读取或通知。需要拆分通知时，每个片段前加四字节二进制头：

```text
byte 0     0xC7
byte 1     sequence：本次快照序号，所有片段一致
byte 2     index：从 0 开始，必须按顺序发送
byte 3     count：片段总数，大于 0，所有片段一致
byte 4...  本片段的文本字节
```

接收端只在最后一片到达后提交完整快照。序号/总数不一致、缺片或乱序会放弃当前组装；
第 0 片开启新组装。设备不返回逐片确认；主机应提供完整可读快照作为恢复路径，
并按连接实际可用通知长度分片，不把固定 MTU 当作保证。

## 心跳、缓存和唤醒边界

- 连接时读取初始快照，之后约每 30 秒补读一次；通知可更及时地下发变化。
- 没有有效命令心跳约 35 秒后，`hostConnected` 变为 false；WORKING 退回 IDLE。
- `connected` 是协议心跳状态，`ble` 是 BLE 链路状态，两者不能混用。
- 未连接时约每 30 秒尝试扫描；约 5 分钟重试失败后熄屏并暂停 BLE，再唤醒可恢复。
- 保留的配额、天气和模型可能来自旧缓存。固件没有通用的指标“过期后清空”机制。
- 主机需自行实现 `activate` 的唤醒/聚焦行为；设备不执行 macOS API。

供桥接程序实现使用的关键入口均在 [src/main.cpp](../src/main.cpp)。
本包不提供 Mac 端安装、签名、公证或权限配置流程，因为配套应用未随仓库发布。
