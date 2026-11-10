# ESP32-S3 PC Clock

这是一个独立的 ESP32-S3 OLED 时钟。开发板不连接 WiFi，也不做 NTP；当前时间由电脑通过 USB 串口发送。项目还加入了 MAX98357A 喇叭、掉电保存闹钟和简单的命令控制台。

## 版本

- 固件：1.3.0（2026-11-09）
- PC 客户端：1.2.0
- 闹钟命令客户端：1.1.0

## 功能

- 128x64 SSD1306 OLED 时钟，显示时、分、秒、日期和星期。
- 时间由电脑通过 USB 串口提供，不需要 WiFi。
- 首次连接立即对时，之后每小时校准一次。
- 如果后续对时失败，ESP32 继续使用上一次同步的时间走时。
- 8 个闹钟槽位，保存到 ESP32 NVS，掉电不丢失。
- 支持每天闹钟和单次闹钟。
- 支持 beep、chime、bird、alarm、custom 五种声音。
- 音量 0-100，响铃时长 1-300 秒。
- 时钟页左下角有小像素狗，右上角有电脑连接图标。
- 可以双击 `alarm_console.cmd` 打开交互式控制台，直接输入命令。

## 硬件

### OLED

| OLED | ESP32-S3 |
| --- | --- |
| SDA | GPIO1 |
| SCL | GPIO2 |
| VCC | 3.3V |
| GND | GND |

OLED 使用 U8g2 的 SSD1306 128x64 硬件 I2C 驱动，速率 400 kHz。

### 喇叭

喇叭输出按 MAX98357A I2S 功放模块设计。它不是无源蜂鸣器，裸喇叭不能直接接到这几个 IO。

| MAX98357A | ESP32-S3 |
| --- | --- |
| BCLK | GPIO4 |
| LRC / WS | GPIO5 |
| DIN | GPIO7 |
| SD | GPIO8 |
| VIN | 5V |
| GND | GND |
| GAIN | GND |

喇叭接到功放的输出端。如果实际接线不同，改固件顶部的 `SPK_BCLK`、`SPK_LRC`、`SPK_DIN`、`SPK_SD` 宏。

## 文件

```text
esp32_pc_clock.ino          ESP32 固件
pc_clock.py                 电脑端串口对时和控制端口
alarm_cli.py                闹钟命令客户端
alarm.cmd                   alarm 快捷命令
alarm_console.cmd           交互式一键控制台
start_clock.cmd             直接启动时钟服务
README.md                   本文件
requirements.txt            pyserial 依赖
build/                      编译产物
```

## 快速开始

1. 按照上面的接线图连接 OLED 和 MAX98357A。
2. 把固件烧录到 ESP32-S3。
3. 双击 `start_clock.cmd`，或者双击 `alarm_console.cmd`。
4. 等待电脑端提示串口连接成功。

电脑端会自动扫描常见 USB 串口，包括 ESP32-S3 原生 USB、CP210x、CH340、FTDI；找不到已知设备时也会继续探测其他串口。

首次对时成功后，ESP32 自己继续走时。电脑之后每小时发送一次新时间即可。

## 一键控制台

双击：

```text
alarm_console.cmd
```

控制台会做这些事：

- 检查时钟服务是否已经运行。
- 如果没有运行，就自动启动 `start_clock.cmd`。
- 等待控制端口和 ESP32 串口连接。
- 进入命令输入界面。
- 输入 `exit` 退出时，时钟服务继续在后台运行。

在控制台里输入命令时，可以不加 `alarm`，也可以加；例如 `add 1:00 once` 和 `alarm add 1:00 once` 都支持。不需要写任何 Python 文件名。直接输入英文命令：

```text
list
add 7:30 daily
add 22:00 once
add 6:45 daily bird 80 20
close alarm1
open alarm1
delete alarm1
test alarm1
stop
exit
```

如果控制台提示还没有检测到串口，插好 ESP32 即可。电脑端服务会自动重新扫描并连接，不需要重启控制台。

## 普通终端命令

`alarm` 已经加入 Windows PATH，在任意终端都能直接使用。如果提示找不到 `alarm`，关闭当前终端再重新打开。

### 查看闹钟

```text
alarm list
alarm show alarms
```

返回示例：

```text
ALARM 0 07:30 daily on chime 70 15
ALARM 1 22:15 once off beep 80 10
OK ALARM LIST 2
```

字段依次是：

```text
slot  time  daily|once  on|off  sound  volume  duration
```

槽位是 `0` 到 `7`。添加成功后返回的编号就是槽位号。

### 添加闹钟

```text
alarm add 7:30 daily
alarm add 22:00 once
alarm add 6:45 daily bird 80 20
alarm add 7:30 daily beep 80 20
alarm set 20:00
```

默认是每天、chime、音量 70、响铃 15 秒。

例如：

```text
alarm add 7:30 daily
```

返回：

```text
OK ALARM ADD 0
```

后面的 `close`、`open`、`delete`、`test` 都用这个槽位号。

支持的声音名：

```text
beep
chime
bird
alarm
custom
```

音量范围是 `0` 到 `100`，响铃时长范围是 `1` 到 `300` 秒。

### 关闭和打开闹钟

```text
alarm close alarm1
alarm open alarm1
```

`close` 只是停用闹钟，闹钟还在列表里；`open` 重新启用。它不会停止正在响的铃声，停止响铃用 `alarm stop`。

### 删除和清空

```text
alarm delete alarm1
alarm remove alarm1
alarm clear
```

`delete` 删除指定槽位；`clear` 删除全部闹钟。

### 试听

```text
alarm test alarm1
alarm test bird 70 5
```

`alarm test alarm1` 会按照该槽位保存的完整设置试听，包括自定义音符。`alarm test bird 70 5` 会试听 bird 声音，音量 70，持续 5 秒。

### 停止响铃

```text
alarm stop
```

### 自定义声音

自定义声音使用音符序列，格式是 `note:milliseconds`，`0` 表示休止：

```text
alarm 3 custom C5:180,E5:180,G5:360,0:120
```

槽位必须是已经存在的闹钟。`custom` 会把该槽位的声音类型改成自定义，并保存音符序列。

### 原始 ALARM 命令

上面的短命令已经覆盖日常使用。脚本需要时仍可使用原始协议：

```text
alarm "ALARM LIST"
alarm "ALARM ADD 07:30 daily chime 70 15"
alarm "ALARM ENABLE 3 0"
alarm "ALARM DEL 3"
alarm "ALARM TUNE 3 C5:180,E5:180,G5:360"
alarm "ALARM TEST 3"
alarm "ALARM STOP"
```

## 闹钟行为

- 最多保存 8 个闹钟。
- 每天闹钟每天同一时间响。
- 单次闹钟在下一个该时刻响一次，然后自动删除。
- 固件只保存 `HH:MM`，单次闹钟不绑定具体日期。如果今天这个时间已经过了，就在明天这个时间响。
- 闹钟设置保存在 ESP32 NVS，掉电不丢失。
- 设备断电时不会响；重新上电后需要先完成时间同步，才能判断闹钟。
- 闹钟调度每分钟检查一次。
- 单次闹钟响过之后会自动从列表中消失。

## 电脑端服务

`start_clock.cmd` 启动串口对时服务。这个服务会：

- 自动查找 ESP32 串口。
- 连接成功后立即发送当前时间和时区偏移。
- 之后每小时发送一次新时间。
- 在本机 `127.0.0.1:8765` 打开控制端口。
- 在对时的同时接受闹钟命令。

如果串口断开，ESP32 继续用上一次同步的时间走时；电脑端会重新扫描，等设备恢复后自动重连。

### 提示控制端口没有打开

先启动服务：

```text
start_clock.cmd
```

然后在另一个终端执行 `alarm` 命令。

### 提示 PC_CLOCK not connected

控制端口已经打开，但电脑还没有连接 ESP32 串口。检查：

1. ESP32 USB 线是否连接。
2. Arduino 串口监视器是否占用 COM 口。
3. COM 口驱动是否安装。
4. 开发板是否插在固件对应的 USB 口上。

电脑端会自动重新扫描，插好设备后不需要重启控制台。

## 编译和烧录

Arduino CLI 示例：

```text
arduino-cli compile --fqbn esp32:esp32:esp32s3 E:\opencode\esp32_pc_clock
```

编译产物：

```text
build\esp32_pc_clock.ino.bin
build\esp32_pc_clock.ino.merged.bin
```

普通 `.bin` 按正常烧录流程烧写，或者把 merged 镜像写到 `0x0`：

```text
esptool.py --chip esp32s3 write_flash 0x0 build\esp32_pc_clock.ino.merged.bin
```

这次只改电脑端闹钟命令，不需要重新编译或烧录固件。当前固件是 1.3.0。

当前构建：

```text
Firmware: 1.3.0
BIN size: 590944 bytes
BIN SHA256: 539E01A4A1767BAFC54B92A30D73329CAB50A40E2B6604E54EAE10C56A5AC7E9
Merged SHA256: A9288B13DE8106053F85EE34C57E001FDED15E5C91B83C01AE570E6E4BE8F69B
```

## 串口协议

电脑通过串口发送以换行结尾的 ASCII 命令：

```text
SYNC <unix-epoch> <timezone-offset-minutes>
PING
ALARM LIST
ALARM ADD HH:MM daily|once [sound] [volume] [duration]
ALARM DEL <slot>
ALARM CLEAR
ALARM ENABLE <slot> 0|1
ALARM TUNE <slot> <note:ms,...>
ALARM TEST <slot>
ALARM TEST [sound] [volume] [duration]
ALARM STOP
```

ESP32 返回 `OK ...`、`ALARM ...` 或 `ERR ...`。

## 常见问题

### 屏幕一直显示 `--:--`

ESP32 还没有收到时间。启动电脑端服务，确认串口连接。检查 COM 口是否可用。

### 时间一直有固定偏差

检查电脑时区。电脑发送的是当前 UTC 偏移，包含夏令时。

### 提示找不到 `alarm`

关闭终端再重新打开，让 Windows 重新加载 PATH。全局命令位于：

```text
C:\Users\35070\AppData\Local\Microsoft\WindowsApps\alarm.cmd
```

### 闹钟不响

检查：

1. 闹钟是否处于启用状态，`alarm list` 中是否显示 `on`。
2. 槽位是否存在，时间是否正确。
3. 开发板是否通电，时间是否已经同步。
4. MAX98357A 接线是否正确，`SD` 是否拉高。
5. 固件是否打印了 `ERR speaker init`。

### 喇叭没有声音

检查功放供电、喇叭极性、`GAIN` 和固件里的 GPIO 编号。无源蜂鸣器直接接 I2S 引脚不会工作。

### `ERR ALARM slot`

槽位超出范围或者为空。用 `alarm list` 查看，然后选择 `0` 到 `7` 中已有的槽位。

## 变更记录

- 固件 1.3.0 / 2026-11-09：加入 MAX98357A 喇叭、掉电保存闹钟、每天和单次闹钟、5 种声音、音量、时长和自定义音符。
- PC 客户端 1.1.0 / 2026-11-09：支持 `alarm list`、`alarm close alarm1`、`alarm add 7:30 daily` 等短命令；新增交互式 `alarm_console.cmd`。
- 1.2.3 / 2026-11-09：修复像素狗偶尔停住；移除夜间睡眠。
- 1.2.2 / 2026-11-09：小狗转身时尾巴自动换边。
- 1.2.1 / 2026-11-09：恢复翻牌滚动和冒号闪烁；整屏约 25fps。
- 1.2.0 / 2026-11-09：移除“PC 已同步”文字，加入 PC 图标，日期移到星期左边，加入像素狗。
- 1.1.0 / 2026-11-09：串口 beacon、自动扫描、每小时校准和 `start_clock.cmd` 一键启动。
- 1.0.0 / 2026-11-09：初始版本，串口对时、OLED 时钟和等待动画。
