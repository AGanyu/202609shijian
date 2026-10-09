# CC2530 路由器与终端 Zigbee 通信示例

这是一个面向 TI Z-Stack 3.0.2 / CC2530DB 的最小双向通信模块。它不复制 Z-Stack SDK，代码通过 GenericApp 的三个钩子接入现有 IAR 工程。C/H 源文件仅使用 ASCII，适合放在现有工程的 `Source` 目录或作为额外 include/source 路径。

## 通信流程

| 方向 | 命令 | 负载 | 说明 |
| --- | --- | --- | --- |
| 路由器 -> 全网 | `0x01 HELLO` | `01,01,sequence` | 路由器每 10 秒广播一次自己的可用状态 |
| 终端 -> 路由器 | `0x02 REPORT` | `02,sequence,temp_hi,temp_lo,rh_hi,rh_lo` | 温度单位为 `0.01 C`，湿度单位为 `0.01 %RH` |
| 路由器 -> 终端 | `0x03 ACK` | `03,sequence,status` | `status=0` 表示接收成功 |

自定义 cluster 为 `0xFC10`，端点为 `8`。终端未收到 ACK 时最多重发 3 次；没有发现路由器时等待下一次 HELLO。路由器地址是动态短地址，不需要写死。

## 接入现有 GenericApp 工程

把 `zb_router_end_device.c` 和 `zb_router_end_device.h` 加入工程，并在 `zcl_genericapp.c` 中完成以下修改：

1. 增加头文件：

   ```c
   #include "zb_router_end_device.h"
   ```

2. 在 `zclGenericApp_Init()` 中调用：

   ```c
   ZbRouterEndDevice_Init();
   ```

3. 在 `AF_INCOMING_MSG_CMD` 分支中调用：

   ```c
   ZbRouterEndDevice_HandleIncoming((afIncomingMSGPacket_t *)MSGpkt);
   ```

4. 在 `ZDO_STATE_CHANGE` 分支中，网络状态更新后调用：

   ```c
   ZbRouterEndDevice_SetNetworkReady(
       (zclGenericApp_NwkState == DEV_ZB_COORD) ||
       (zclGenericApp_NwkState == DEV_ROUTER) ||
       (zclGenericApp_NwkState == DEV_END_DEVICE));
   ```

5. 在 `zclGenericApp_event_loop()` 的普通事件处理区调用：

   ```c
   events = ZbRouterEndDevice_ProcessEvent(events);
   ```

   建议放在处理 `SHT30_MEASURE_EVT` 之前。函数会只清除本模块已经处理的事件位。

6. 在 `zcl_genericapp_data.c` 中让端点描述声明自定义 cluster。在输入和输出 cluster 数组中都加入：

   ```c
   #include "zb_router_end_device.h"
   // zclGenericApp_InClusterList 和 zclGenericApp_OutClusterList 中加入
   ZB_RE_CLUSTER_ID,
   ```

## 选择固件角色

在 IAR 工程的对应 configuration 预处理宏中定义一个角色：

```text
ZB_NODE_ROLE=ZB_ROLE_ROUTER       // 路由器固件
ZB_NODE_ROLE=ZB_ROLE_END_DEVICE   // 终端固件
```

推荐直接复制现有工程的 `RouterEB` 和 `EndDeviceEB` configuration，然后在 IAR 的 C/C++ Compiler -> Preprocessor -> Defined symbols 中分别加入 `ZB_NODE_ROLE=ZB_ROLE_ROUTER` 或 `ZB_NODE_ROLE=ZB_ROLE_END_DEVICE`。两块 CC2530 使用相同的 PAN/channel 和安全配置，并先让协调器允许入网。

## DHT11 接线与读取

终端固件已集成 DHT11 单总线读取，DATA 固定接 CC2530 `P0.6`，VCC 接 `3.3V`，GND 接 `GND`，DATA 与 3.3V 之间外接 `4.7kΩ~10kΩ` 上拉电阻。DHT11 至少每 1 秒读取一次，本示例按 30 秒上报周期读取，满足要求。

代码文件为 `dht11.c` 和 `dht11.h`，需要加入 IAR 工程。读取失败或校验和错误时本周期不上报，下一周期自动重试。

## 传感器数据接口

如果不使用 DHT11，可在采样完成处调用以下接口覆盖上报值：

```c
ZbRouterEndDevice_SetMeasurement(temperature_centi_c, humidity_centi_pct);
```

例如 `2534` 表示 `25.34 C`，`4860` 表示 `48.60 %RH`。路由器侧可用 `ZbRouterEndDevice_GetLastReport()` 读取最近一次上报。

## 注意

- 本目录是应用层源码，编译必须依赖 TI Z-Stack 头文件、库文件和 IAR 8051 工具链。
- 不能把本模块当作可独立编译的 8051 工程；它需要放进已有 `GenericApp`/`CC2530DB` 工程。
- 短地址会在入网后动态分配，HELLO 机制避免了把路由器地址硬编码为 `0x0001`。
- 如果工程启用了低功耗终端休眠，请把 `ZB_RE_REPORT_EVT` 的周期和轮询参数调整为设备的唤醒窗口；本示例默认按常供电终端处理。
