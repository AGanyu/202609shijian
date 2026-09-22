# 智仓卫士电脑后端

这是成员6负责的电脑后端，按《API设计文档 v1.0》实现。服务使用 Python 标准库、SQLite 和 HTTP/1.1，不需要额外安装第三方依赖。

## 启动

在项目根目录执行：

```powershell
python -m backend
```

默认监听：

```text
http://0.0.0.0:8000
```

默认开发网关配置：

```text
gateway_id: esp32-gateway-001
api_key: dev-gateway-key
```

正式联调时请通过环境变量替换密钥：

```powershell
$env:GATEWAY_API_KEYS='{"esp32-gateway-001":"replace-with-real-key"}'
python -m backend
```

也可以调整数据库、日志目录和监听端口：

```powershell
$env:PORT='8000'
$env:DATABASE_PATH='data/cold_chain.db'
$env:LOG_DIR='data/logs'
```

## 已实现接口

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/telemetry` | 网关上报单条采样数据 |
| `POST` | `/api/v1/telemetry/batch` | 网关批量补传数据 |
| `POST` | `/api/v1/gateways/heartbeat` | 网关心跳 |
| `GET` | `/api/v1/devices` | 查询设备列表 |
| `GET` | `/api/v1/telemetry/latest` | 查询各节点最新数据 |
| `GET` | `/api/v1/telemetry/history` | 查询历史采样数据 |
| `GET` | `/api/v1/alerts` | 查询告警 |
| `POST` | `/api/v1/alerts/{alert_id}/ack` | 确认告警 |
| `GET` | `/api/v1/health` | 服务和数据库健康检查 |

## 联调示例

检查服务：

```powershell
Invoke-RestMethod http://127.0.0.1:8000/api/v1/health
```

发送网关心跳：

```powershell
$headers = @{
  'Content-Type' = 'application/json'
  'X-Gateway-Id' = 'esp32-gateway-001'
  'X-Api-Key' = 'dev-gateway-key'
}
$heartbeat = @{
  gateway_id = 'esp32-gateway-001'
  firmware_version = '0.1.0'
  ip_address = '192.168.1.20'
  uptime_seconds = 3600
  connected_nodes = 3
  free_heap = 182432
  sent_at = '2026-09-19T07:30:00Z'
} | ConvertTo-Json
Invoke-RestMethod `
  -Method Post `
  -Uri http://127.0.0.1:8000/api/v1/gateways/heartbeat `
  -Headers $headers `
  -Body $heartbeat
```

发送采样数据：

```powershell
$telemetry = @{
  message_id = 'esp32-gateway-001-20260919073000001'
  gateway_id = 'esp32-gateway-001'
  device_id = 'zb-node-001'
  zone_id = 'cold-storage-a'
  collected_at = '2026-09-19T07:30:00Z'
  received_at = '2026-09-19T07:30:01Z'
  temperature = -18.6
  humidity = $null
  signal = -62
  quality = 'normal'
} | ConvertTo-Json
Invoke-RestMethod `
  -Method Post `
  -Uri http://127.0.0.1:8000/api/v1/telemetry `
  -Headers $headers `
  -Body $telemetry
```

## 数据和日志

首次启动会创建：

```text
data/cold_chain.db
data/logs/requests.jsonl
data/logs/telemetry.jsonl
```

默认预置 3 个测试节点：

```text
zb-node-001  cold-storage-a
zb-node-002  cold-storage-a
zb-node-003  cold-storage-b
```

`message_id` 是 SQLite 主键，重复上报返回 HTTP `200` 和 `duplicate: true`。请求日志不记录 API Key 原文，原始采样日志也会把密钥标记为 `[redacted]`。

## 测试

```powershell
python -m unittest discover -s tests -v
```

测试覆盖健康检查、设备查询、单条上报、批量补传、重复数据、历史查询、心跳在线状态、告警生成与确认、鉴权和字段校验。
