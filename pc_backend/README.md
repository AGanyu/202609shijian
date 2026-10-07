# ESP32-S3 WiFi 串口桥接后端

电脑端后端使用 Python 标准库，不需要安装第三方包。它监听 HTTP `8080` 端口，接收 ESP32 的公网 HTTPS POST，并提供浏览器监控页面。

## 启动

在 PowerShell 7 中执行：

```powershell
python .\server.py
```

浏览器打开：<http://127.0.0.1:8080/>。局域网其他电脑可打开 `http://电脑IP:8080/`。

可指定端口：

```powershell
python .\server.py --host 0.0.0.0 --port 8765
```

修改网页端口：

```powershell
python .\server.py --web-host 0.0.0.0 --web-port 8080
```

启动后输入：

- `send hello`：把 `hello` 放入所有已登记 ESP32 的 HTTP 命令队列；
- `list`：列出当前已登记的 ESP32；
- `quit`：停止后端。

网页会显示在线 ESP32 数量、已解析的传感模块、温度、湿度、更新时间和最近串口消息；“发送串口命令”可向在线 ESP32 广播一行文本。

樱花隧道应将公网入口转发到电脑的 `127.0.0.1:8080`。ESP32 和电脑不需要在同一个局域网。

## ESP32 配置

编辑上级目录的 `esp32_s3_wifi_serial.ino`：

```cpp
const char *WIFI_SSID = "你的WiFi名称";
const char *WIFI_PASSWORD = "你的WiFi密码";
```

默认外部 UART 为 `GPIO17` (RX)、`GPIO18` (TX)、115200 8N1；如接线不同，修改 `SERIAL_RX_PIN`、`SERIAL_TX_PIN` 和 `DEVICE_SERIAL_BAUD`。USB CDC `Serial` 仅用于调试日志，外部串口数据通过 `DeviceSerial` 转发。

传感器 UART 使用 `GPIO4` (RX)、`GPIO5` (TX)、`9600 8N1`。设备若只自动输出数据，只需要连接：设备 TX -> ESP32 GPIO4，设备 GND -> ESP32 GND；双方必须是 3.3V 电平。程序识别 `R:055.4RH 026.5C`，并转换为模块 `R` 的湿度和温度数据。

传感器数据默认通过公网 HTTPS POST 上报到：

```text
https://www.u500703.nyat.app:63358/api/telemetry
```

ESP32 程序中的 `HTTP_TELEMETRY_URL` 和 `HTTP_COMMANDS_URL` 指向公网入口。樱花隧道应将公网端口映射到电脑 `127.0.0.1:8080`。前端通过同一公网域名的相对路径访问 API。

公网接口要求请求头：

```text
X-Device-Token: u500703-esp32-telemetry-2026-change-me
```

HTTP POST 请求体示例：

```json
{"type":"serial","data":{"module":"R","temperature":26.5,"humidity":55.4,"raw":"R:055.4RH 026.5C"}}
```

调试模拟已关闭，`DEBUG_SENSOR_TEST` 为 `false`，当前只上报真实传感器数据。

板卡选择：`ESP32S3 Dev Module`，FQBN 为 `esp32:esp32:esp32s3`。本工程使用 Arduino-ESP32 3.x，自带 `WiFi.h`；`ArduinoJson` 用于桥接协议解析。

编译和烧录示例（把 `COMx` 换成实际端口）：

```powershell
$cli = 'D:\ArduinoTools\arduino-cli-0.35.3\arduino-cli.exe'
$cfg = 'D:\ArduinoTools\ArduinoCLIConfig\arduino-cli.yaml'
$sketch = 'D:\20269proj\esp32_s3_wifi_serial'
& $cli --config-file $cfg compile --fqbn esp32:esp32:esp32s3 $sketch
& $cli --config-file $cfg upload -p COMx --fqbn esp32:esp32:esp32s3 $sketch
```

如果你的 WROOM-1 是带 16MB Flash 和 OPI PSRAM 的版本，应把 FQBN 改为：

```text
esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc
```

## 通信协议

ESP32 发给后端的每行是一个 JSON 对象：

```json
{"type":"serial","data":"来自外部串口的一行","millis":12345}
```

后端发给 ESP32 的命令格式：

```json
{"type":"command","data":"要写入外部串口的一行"}
```

传感器串口以换行作为一条消息的结束符；单行最大 768 字节。公网数据使用 HTTPS POST，命令使用 HTTPS 轮询。

## 传感器数据格式

推荐让外部传感器控制器通过 UART 每行发送 JSON，例如：

```json
{"module":"仓库1","temperature":26.4,"humidity":58.2}
```

也支持文本键值格式：

```text
module=仓库1 temperature=26.4 humidity=58.2
```

后端会按 `module`（也支持 `sensor`、`name`、`id`）区分模块；温度支持 `temperature`/`temp`，湿度支持 `humidity`/`hum`/`rh`。只有包含温度或湿度的串口行才会进入模块卡片。
