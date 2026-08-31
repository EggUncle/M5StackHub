# 上手、编译与安全烧录

[返回首页](../README.md)

## 1. 确认范围

本包针对项目中已验证的 M5Stack StopWatch / ESP32-S3，16MB Flash、8MB PSRAM。
`esp32s3box` 只是沿用的 PlatformIO 构建 profile，不是适用硬件清单。
不要据此刷写 ESP32-S3-BOX、StackChan、CoreS3 或其他板型。

需要一根支持数据传输的 USB 线，以及 Python 3.11 / PlatformIO 6.1.18 构建环境。
首次编译需要联网下载工具链和固定版本的依赖。文档配图使用 Node.js，固件编译不需要它。

这个包不包含 Mac 端 M5StackHub。没有兼容桥接程序时，可以通过 USB 命令检查显示和协议，
但不会自动同步 Codex、天气或系统指标。

## 2. 编译

在仓库目录运行：

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install platformio==6.1.18
pio run -e stopwatch_pet
```

配置入口为 [platformio.ini](../platformio.ini)：

- Espressif32 平台：6.12.0。
- Arduino framework，ESP32-S3，QIO/OPI 内存设置。
- M5GFX：官方 0.2.26 标签对应的固定 Git 提交。
- M5Unified：固定 Git 提交。
- `CODEX_PET_STOPWATCH=1` 选择 StopWatch 分支。

环境名 `stopwatch_pet` 和协议名 `codex-pet/1` 为历史兼容名称，**不表示界面仍显示宠物**。
不要删除宏或把环境改成其他硬件来尝试“修复”编译问题。

产物在 `.pio/build/stopwatch_pet/`，主要包括 `firmware.bin`、`bootloader.bin` 和 `partitions.bin`。
`firmware.bin` 是应用镜像，不是可直接写到地址 0 的整片 Flash 镜像。
推荐让本工程的 PlatformIO upload 目标处理布局，不手工猜测偏移量。

## 3. 烧录前检查

1. 先确认设备型号、USB 端口及现有固件来源。插拔前后对比 `pio device list`。
2. 如果原固件还需要保留，先安排与该硬件匹配的备份；本包不包含用户 Flash/NVS 备份。
3. 退出正在占用端口的串口监视器或桥接进程。
4. 确保供电稳定。烧录会覆盖应用固件；不要执行整片擦除，不要导入别的设备备份。

```sh
pio device list
# 替换为刚确认的设备端口；此字符串只是示例，不可原样使用。
export STOPWATCH_PORT='/dev/cu.usbmodemXXXX'
pio run -e stopwatch_pet -t upload --upload-port "$STOPWATCH_PORT"
```

平台配置同时指定分区布局；即使不擦除整片 Flash，也不保证跨不同分区方案保留原数据。
如果现有布局不明，先停止并核实。

## 4. 启动检查

```sh
pio device monitor -p "$STOPWATCH_PORT" -b 115200
```

通过支持发送文本行的串口终端发送以下命令，每条以换行结束：

```text
hello
status
```

应看到 `PONG codex-pet/1 pet=stopwatch`，以及 `STATUS` 和 `WEATHER` 行。
`status` 是诊断请求，不会获取新的 Codex 配额或天气。
发出 USB 命令会刷新固件的主机心跳时间，因此 `connected=1` 本身不代表 BLE 已连接；
要单独查看 `ble=1`。

可按需用下列命令测试状态，然后让正常的主机同步恢复真实状态：

```text
wake
state running
state ready
```

这些命令会改变设备显示；部分设置会写入 NVS。这里只提供测试方法，打包过程不会自动发送。
其他字段及示例见 [协议文档](protocol.md)。

## 5. 正常使用

启动兼容的 Mac 端桥接程序，让它通过 BLE 广播服务并提供快照。StopWatch 主动扫描并连接。
预期会出现 `BLE host ready`、`BLE snapshot applied bytes=...`；随后核对日期、配额、CPU、内存。
天气位置标签为“余杭”，主机端也必须配置为杭州余杭，标签不会校验收到的数据来自哪里。

触摸/按键、休眠行为和状态含义见 [界面说明](interface.md)。

## 自动构建

[GitHub Actions 配置](../.github/workflows/build.yml) 在 push、pull request 或手动触发时编译，
并保留二进制 artifact。本地编译记录见 [验证记录](verification.md)，云端结果以
[GitHub Actions](https://github.com/EggUncle/M5StackHub/actions/workflows/build.yml) 为准。
它不自动创建 Release、不上传到设备，也不发布 Mac 应用。
