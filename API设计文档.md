# 智仓卫士 API 设计文档
  
**文档版本：** v1.0  
**适用范围：** ESP32 网关、电脑后端、Web 前端、Linux 辅助调试设备和樱花 FRP 远程访问

## 1. 设计依据

本接口设计按照项目当前确定的设备关系制定：

```text
传感器 → ESP32 节点 → ESP32 网关
                         ├─ Wi-Fi / HTTP POST → 电脑后端
                         └─ 调试日志 → Linux 设备

电脑后端 ← 公网 / 樱花 FRP 域名 → Web 前端
```

接口边界如下：

- 传感器和 ZigBee 节点负责采集与组网，不直接访问公网。
- ESP32 网关负责接收节点数据、打包数据、添加时间戳，并通过 Wi-Fi 发送到电脑后端。
- 电脑后端负责接收数据、校验数据、记录日志、提供查询接口和告警数据。
- Web 前端只调用电脑后端 API，不直接访问 ESP32、传感器或 Linux 设备。
- Linux 设备用于日志记录、断网模拟、数据回放和故障调试，不参与正常业务主链路。
- 樱花 FRP 只负责把电脑上的 Web/API 服务映射到公网域名，不属于业务数据协议。

## 2. 技术约定

| 项目 | 约定 |
|---|---|
| 应用协议 | HTTP/1.1 |
| 网关上报方式 | `POST` + `application/json` |
| 前端查询方式 | `GET` + JSON |
| 后端控制方式 | `POST` |
| 字符编码 | UTF-8 |
| 时间格式 | ISO 8601，统一使用 UTC，例如 `2026-09-18T07:30:00Z` |
| 温度单位 | 摄氏度，单位 `°C` |
| 湿度单位 | `%RH`，没有湿度传感器时字段传 `null` |
| 数据编号 | 网关生成 `message_id`，后端按编号去重 |
| API 前缀 | `/api/v1` |
| 成功响应 | HTTP `200` 或 `201` |
| 客户端错误 | HTTP `400`、`401`、`404`、`409` |
| 服务端错误 | HTTP `500`、`503` |

## 3. 地址和访问方式

### 3.1 本地开发地址

电脑后端默认监听：

```text
http://电脑局域网IP:8000
```

示例：

```text
http://192.168.1.100:8000
```

### 3.2 樱花 FRP 远程地址

樱花 FRP 将电脑后端的 `8000` 端口映射到远程域名：

```text
https://项目域名/api/v1/...
```

FRP 只改变访问入口，API 路径、请求体和返回格式保持不变。网关在现场调试时优先使用电脑局域网地址，不能依赖 FRP 域名完成采集。

## 4. 数据模型

### 4.1 设备信息 `Device`

```json
{
  "device_id": "zb-node-001",
  "gateway_id": "esp32-gateway-001",
  "zone_id": "cold-storage-a",
  "device_type": "zigbee_sensor",
  "sensor_type": "SHT30",
  "enabled": true
}
```

字段说明：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `device_id` | string | 是 | ZigBee 节点唯一编号 |
| `gateway_id` | string | 是 | 所属 ESP32 网关编号 |
| `zone_id` | string | 是 | 仓库区域编号 |
| `device_type` | string | 是 | 固定为 `zigbee_sensor` |
| `sensor_type` | string | 是 | 当前使用 `SHT30`，如更换传感器需同步更新文档 |
| `enabled` | boolean | 是 | 是否参与采集 |

### 4.2 采样数据 `Telemetry`

```json
{
  "message_id": "esp32-gateway-001-20260918073000001",
  "gateway_id": "esp32-gateway-001",
  "device_id": "zb-node-001",
  "zone_id": "cold-storage-a",
  "collected_at": "2026-09-18T07:30:00Z",
  "received_at": "2026-09-18T07:30:01Z",
  "temperature": -18.6,
  "humidity": null,
  "signal": -62,
  "quality": "normal"
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `message_id` | string | 是 | 单条数据唯一编号，用于幂等去重 |
| `gateway_id` | string | 是 | ESP32 网关编号 |
| `device_id` | string | 是 | 采集节点编号 |
| `zone_id` | string | 是 | 监测区域编号 |
| `collected_at` | string | 是 | 传感器采样时间 |
| `received_at` | string | 否 | 网关接收时间，由网关填写 |
| `temperature` | number | 是 | 温度值，范围建议为 `-50` 到 `80` |
| `humidity` | number/null | 否 | 湿度值，没有湿度传感器时为 `null` |
| `signal` | integer | 否 | ZigBee 信号强度，单位 dBm |
| `quality` | string | 是 | `normal`、`warning` 或 `invalid` |

### 4.3 告警信息 `Alert`

```json
{
  "alert_id": "alert-202609180001",
  "message_id": "esp32-gateway-001-20260918073000001",
  "device_id": "zb-node-001",
  "zone_id": "cold-storage-a",
  "type": "temperature_high",
  "level": "warning",
  "value": -8.2,
  "threshold": -18.0,
  "status": "open",
  "created_at": "2026-09-18T07:35:00Z"
}
```

## 5. ESP32 网关 API

### 5.1 上报单条采样数据

```http
POST /api/v1/telemetry
Content-Type: application/json
X-Gateway-Id: esp32-gateway-001
X-Api-Key: <gateway-api-key>
```

请求体使用 `Telemetry` 对象。后端处理顺序：

1. 校验 API Key、网关编号和字段格式。
2. 检查 `message_id` 是否已经存在。
3. 校验温度、湿度、时间戳和设备状态。
4. 写入数据库和原始日志。
5. 根据阈值生成或更新告警。

成功响应：

```json
{
  "code": 0,
  "message": "accepted",
  "data": {
    "message_id": "esp32-gateway-001-20260918073000001",
    "stored": true,
    "duplicate": false
  }
}
```

重复数据响应仍返回 HTTP `200`，但 `duplicate` 为 `true`，避免网关因为重试而产生重复日志：

```json
{
  "code": 0,
  "message": "already accepted",
  "data": {
    "message_id": "esp32-gateway-001-20260918073000001",
    "stored": false,
    "duplicate": true
  }
}
```

### 5.2 批量补传数据

当电脑后端短暂不可用，ESP32 可以在本地缓存少量数据，恢复连接后批量补传：

```http
POST /api/v1/telemetry/batch
Content-Type: application/json
X-Gateway-Id: esp32-gateway-001
X-Api-Key: <gateway-api-key>
```

请求体：

```json
{
  "gateway_id": "esp32-gateway-001",
  "items": [
    {
      "message_id": "msg-001",
      "device_id": "zb-node-001",
      "zone_id": "cold-storage-a",
      "collected_at": "2026-09-18T07:30:00Z",
      "temperature": -18.6,
      "humidity": null,
      "quality": "normal"
    }
  ]
}
```

约束：单次最多 `100` 条，后端逐条按 `message_id` 去重，返回成功、重复和失败编号。

### 5.3 网关心跳

```http
POST /api/v1/gateways/heartbeat
Content-Type: application/json
X-Gateway-Id: esp32-gateway-001
X-Api-Key: <gateway-api-key>
```

请求体：

```json
{
  "gateway_id": "esp32-gateway-001",
  "firmware_version": "0.1.0",
  "ip_address": "192.168.1.20",
  "uptime_seconds": 3600,
  "connected_nodes": 3,
  "free_heap": 182432,
  "sent_at": "2026-09-18T07:30:00Z"
}
```

心跳用于前端显示网关在线状态，不承载采样数据。建议网关每 `30` 秒发送一次，连续 `3` 次未收到时标记为离线。

## 6. 前端查询 API

### 6.1 查询设备列表

```http
GET /api/v1/devices?zone_id=cold-storage-a&enabled=true
```

### 6.2 查询实时数据

```http
GET /api/v1/telemetry/latest?zone_id=cold-storage-a
```

返回：

```json
{
  "code": 0,
  "message": "ok",
  "data": [
    {
      "device_id": "zb-node-001",
      "zone_id": "cold-storage-a",
      "temperature": -18.6,
      "humidity": null,
      "quality": "normal",
      "collected_at": "2026-09-18T07:30:00Z",
      "online": true
    }
  ]
}
```

### 6.3 查询历史数据

```http
GET /api/v1/telemetry/history?device_id=zb-node-001&start=2026-09-18T00:00:00Z&end=2026-09-18T08:00:00Z&page=1&page_size=100
```

参数约束：

- `device_id` 和时间范围至少提供一个设备或区域过滤条件。
- `end` 必须大于 `start`。
- 单次查询时间跨度不超过 `7` 天。
- `page_size` 范围为 `1` 到 `500`。

### 6.4 查询告警

```http
GET /api/v1/alerts?status=open&level=warning&page=1&page_size=20
```

### 6.5 确认告警

```http
POST /api/v1/alerts/{alert_id}/ack
Content-Type: application/json
```

请求体：

```json
{
  "operator": "member7",
  "remark": "已检查冷藏区设备，正在恢复温度"
}
```

## 7. 后端健康检查 API

### 7.1 服务健康检查

```http
GET /api/v1/health
```

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "service": "cold-chain-backend",
    "status": "up",
    "database": "up",
    "timestamp": "2026-09-18T07:30:00Z"
  }
}
```

该接口供成员6、成员7和成员8联调使用，也可作为 FRP 远程访问是否成功的检查入口。

## 8. 统一错误格式

所有错误响应使用以下格式：

```json
{
  "code": 1002,
  "message": "invalid temperature",
  "data": null,
  "request_id": "req-202609180001"
}
```

错误码约定：

| 错误码 | HTTP | 含义 | 处理方 |
|---:|---:|---|---|
| `0` | 200/201 | 成功 | 调用方继续处理 |
| `1001` | 400 | JSON 格式错误 | ESP32 修正请求体 |
| `1002` | 400 | 温度、湿度或时间戳非法 | 成员3/5检查采集和解析 |
| `1003` | 400 | 缺少必填字段 | 成员2/5检查接口实现 |
| `1004` | 401 | API Key 无效 | 成员5检查配置 |
| `1005` | 404 | 设备或告警不存在 | 成员6检查数据库 |
| `1006` | 409 | 设备编号或配置冲突 | 成员3/4/6共同处理 |
| `1007` | 429 | 请求频率过高 | ESP32降低重试频率 |
| `2001` | 500 | 数据库写入失败 | 成员6处理 |
| `2002` | 503 | 服务暂时不可用 | 成员6/8进行断网调试 |

## 9. 安全和可靠性要求

1. 网关上报接口必须携带 `X-Api-Key`，每个网关使用独立密钥，不把密钥写入前端代码。
2. 后端只接受已登记的 `gateway_id` 和 `device_id`。
3. 后端以 `message_id` 去重，网关重试不会生成重复记录。
4. ESP32 请求超时后采用间隔重试，建议最多重试 `3` 次，失败数据写入本地缓存。
5. 后端日志至少记录请求时间、来源 IP、网关编号、接口路径、HTTP 状态码和错误码，不记录 API Key 原文。
6. FRP 外网访问只开放必要端口，答辩前必须保留电脑局域网地址作为备用入口。
7. Linux 断网调试只改变测试环境，不应修改生产数据库中的原始采样记录。

## 10. 成员交接关系

| 交接 | 上游成员 | 下游成员 | 交接内容 | 验收方式 |
|---|---|---|---|---|
| 传感器采集 | 成员3 | 成员4、5 | 节点编号、采样周期、样例数据 | 串口输出和节点清单 |
| ZigBee通信 | 成员4 | 成员5 | 数据帧、信号状态、掉线规则 | 连续接收 10 条数据 |
| 网关上报 | 成员5 | 成员6 | URL、请求头、JSON 报文、重试规则 | 后端收到并保存数据 |
| 后端接口 | 成员6 | 成员7 | 查询路径、字段、分页和错误码 | 前端显示真实数据 |
| FRP访问 | 成员7 | 成员8 | 域名、映射端口、备用本地地址 | 本地和远程均能访问 |
| 测试反馈 | 成员8 | 相关负责人 | 缺陷步骤、日志、复测结果 | 缺陷关闭并记录证据 |

## 11. 联调顺序和验收用例

联调必须按以下顺序执行：

1. 成员3确认传感器能产生有效温度数据。
2. 成员4确认 ZigBee 节点入网、通信和掉线恢复。
3. 成员5用固定测试报文调用 `/api/v1/telemetry`。
4. 成员6确认后端返回 `200`，数据库产生一条记录。
5. 成员7调用 `/telemetry/latest`，页面显示与数据库一致的温度。
6. 成员7配置 FRP，成员8从远程域名访问 `/api/v1/health`。
7. 成员8断开电脑网络或停止后端，检查网关重试、缓存和日志。
8. 服务恢复后，调用批量接口补传，确认重复数据不会重复入库。

最低验收标准：

- 连续采集并保存不少于 `10` 条有效数据。
- 至少显示 `3` 个监测节点的最新数据。
- 节点掉线后能在日志中定位设备编号和时间。
- 后端重启后历史数据仍可查询。
- FRP 域名可访问前端或健康检查接口。
- 本地地址和远程域名均准备好，远程访问失败不影响核心功能演示。

