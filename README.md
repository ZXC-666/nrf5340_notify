# nRF5340 通知投屏

把 Android 手机的通知通过 BLE 推到 nRF5340 DK，用 LVGL 渲染到 ILI9341 屏幕上。

手机侧使用 **Gadgetbridge（Bangle.js 版）**，设备端通过 **Nordic UART Service (NUS)** 接收 Gadgetbridge 的 JSON 通知包。

## 功能

- 手机通知实时投屏：App 来源、标题、发送者、正文
- 中文显示（思源黑体 16px 子集，约 1187 个常用字 + 全部 ASCII）
- 深色 UI：顶部 App 条 + 彩色首字头像 / 常用类别符号图标
- 自动重新广播：断开后自动恢复，可重复连接
- BLE 配对信息持久化到 NVS，重启不掉绑定

## 硬件

| 信号 | nRF5340 DK |
|---|---|
| SCK | D13 / P1.15 |
| MOSI | D11 / P1.13 |
| CS | D10 / P1.12 |
| DC | D9 / P1.11 |
| RESET | D8 / P1.10 |
| VCC / GND | 3V3 / GND |

屏幕为 ILI9341 320×240（横屏使用），RGB565。

## 构建与烧录

依赖 nRF Connect SDK v3.4.0。

```bash
west build -p always -d build_1
west flash -d build_1
```

nRF5340 是双核芯片，BLE 控制器跑在网络核（cpunet）。仓库里的 `Kconfig.sysbuild` 会生成网络核镜像；`west flash` 会按 `domains.yaml` 依次烧录两个核。**只烧应用核的 hex 会导致设备能显示但无法广播。**

## 手机侧配置

1. 安装 **Bangle.js 版 Gadgetbridge**（包名 `com.espruino.gadgetbridge.banglejs`），并授予通知使用权
2. 打开设置里的 **Discover unsupported devices**（发现不支持的设备）
3. 扫描并添加 `Bangle.js` 设备
4. 连接后触发一条通知即可看到屏幕更新

设备名固定为 `Bangle.js` 是刻意的：Gadgetbridge 按名称识别 Bangle.js 设备，只有匹配该名称才会自动把它当作受支持设备并转发通知。

## 协议

Gadgetbridge 向 NUS RX 写入以换行结尾的 JSON：

```json
{"t":"notify","id":123,"src":"qq","sender":"Alice","title":"Hello","body":"世界"}
```

设备端按 `\n` / `\r` 分帧，解析 `src` / `sender` / `title` / `body`，并处理 `\uXXXX`（含代理对）与常见转义。其他类型（如 `is_gps_active`、`notify-remove`）直接忽略。

调试时也可以用 nRF Connect 手动向 RX 写入同样的 JSON（记得勾选换行）。

## 显示适配说明

这块 ILI9341 模组有两处与常规不同的特性，都在 `src/main.c` 的 `mirror_flush_cb()` 里处理：

1. **物理水平镜像**：面板显示方向与显存相反，刷新时逐行反转像素并翻转目标 X 坐标
2. **字节序**：面板按高字节优先接收 RGB565，而 LVGL 输出小端，因此发送前做字节交换

没有修改 Zephyr SDK 的 `display_ili9xxx.c`，全部在应用层通过覆盖 flush 回调实现。

## 已知限制

- 中文字体为子集，生僻字会显示为空白方块
- Gadgetbridge 的 Bangle.js 通道只传文本，无法获取真实 App 图标，因此用首字头像或内置符号替代
- 全片擦除（`nrfjprog --eraseall`）后配对信息丢失，手机需重新配对
- `CONFIG_BT_MAX_PAIRED=1`，只有一个配对槽位

## 目录结构

```
src/main.c                                   通知解析 + LVGL UI + flush 适配
boards/nrf5340dk_nrf5340_cpuapp.overlay      ILI9341 SPI 设备树
prj.conf                                     功能与驱动配置
Kconfig.sysbuild                             启用网络核 BLE 控制器镜像
```
