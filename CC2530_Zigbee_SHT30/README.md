# CC2530 Zigbee + SHT30

基于本机 `D:\Z-Stack 3.0.2` 和 IAR Embedded Workbench for 8051 10.10 的 `CC2530DB` 工程。工程保留 Z-Stack 的 BDB 入网流程，通过 UART 接收串口版 SHT30 的 ASCII 数据，再通过 Zigbee AF 自定义报文发送到协调器。

## IAR 工程

- 工程：`CC2530_Zigbee_SHT30.eww`
- 工程文件：`CC2530_Zigbee_SHT30.ewp`
- IAR IDE：`D:\Embedded Workbench 8.0\common\bin\IarIdePm.exe`
- 命令行构建：`D:\Embedded Workbench 8.0\common\bin\IarBuild.exe`
- 已验证配置：`CoordinatorEB`、`RouterEB`、`EndDeviceEB`

## 串口模块接线

图片中的模块是串口版传感器，实测输出格式为 `R:055.4RH 026.5C\\r\\n`，串口参数为 `9600 8N1`。模块引脚按图片丝印从上到下为：

| 模块 | CC2530 |
|---|---|
| GND | GND |
| TX | UART1 RX（默认 P1.6） |
| RX | UART1 TX（默认 P1.7） |
| 5V | 5V |

模块自动周期输出数据，CC2530 使用 UART1 解析 `R:<humidity>RH <temperature>C`。UART0 保留给协调器到 ESP32 的网关串口。模块的 UART 电平需要确认是否为 3.3V TTL；如果 TX 为 5V，必须先经过电平转换后再接 CC2530。

已在电脑 `COM4` 以 `9600 8N1` 抓到实际数据：`R:055.4RH 026.5C`，解析结果为湿度 `55.4%RH`、温度 `26.5 C`。

## Zigbee 数据

- 源端点：8
- 目标短地址：`0x0000`（协调器）
- 目标端点：8
- 自定义 cluster：`0xFC00`
- 周期：30 秒
- 负载 7 字节：`version, status, temperature_hi, temperature_lo, humidity_hi, humidity_lo, sequence`
- 温度单位：摄氏度的百分之一，例如 `2534` 表示 `25.34 C`
- 湿度单位：相对湿度的百分之一，例如 `4860` 表示 `48.60 %RH`

`status=0` 表示串口数据解析成功；非 0 表示尚未收到有效数据。

## CC2530 协调器与 ESP32 串口网关

`Source/gateway_uart.c` 只在 `CoordinatorEB` 中启用，使用 CC2530 `HAL_UART_PORT_0`、
115200-8-N-1、无硬件流控。CC2530 的 UART0 TX 接 ESP32 外部 UART RX，UART0 RX 接 ESP32
外部 UART TX，双方共地且必须都是 3.3 V TTL；具体 CC2530DB 引脚复用按开发板原理图确认。

协调器收到 `0xFC00` cluster 的 ZigBee 数据后，通过 UART 输出一行：

```text
ZB,src=0x1234,len=7,lqi=92,rssi=-61,data=010009E613F401
```

其中 `data` 保持现有 7 字节格式：`version,status,temperature_hi,temperature_lo,
humidity_hi,humidity_lo,sequence`。UART 接收支持以下命令：

```text
PING
SEND 1234 01010000000002
```

`PING` 返回 `PONG`；`SEND` 的第一个参数是 4 位十六进制短地址，第二个参数是要发送到
该节点的十六进制 AF 负载，成功返回 `TX_OK`，失败返回 `TX_ERR,<状态码>`。命令必须以换行
结束，最大 143 个字符。ESP32 只需把现有串口桥接链路中的下行文本原样写入 CC2530 UART，
不需要在 CC2530 侧解析 JSON。

## 构建

```powershell
& 'D:\Embedded Workbench 8.0\common\bin\IarBuild.exe' '.\CC2530_Zigbee_SHT30.ewp' -build CoordinatorEB
& 'D:\Embedded Workbench 8.0\common\bin\IarBuild.exe' '.\CC2530_Zigbee_SHT30.ewp' -build RouterEB
& 'D:\Embedded Workbench 8.0\common\bin\IarBuild.exe' '.\CC2530_Zigbee_SHT30.ewp' -build EndDeviceEB
```

烧录前请按实际硬件选择对应配置和下载方式；本工程只完成编译验证，未连接实物做传感器波形和入网测试。
