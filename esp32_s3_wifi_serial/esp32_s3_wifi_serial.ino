// ESP32-S3 WiFi/TCP <-> UART bridge.
// Wire the external serial device to SERIAL_RX_PIN/SERIAL_TX_PIN and
// configure the WiFi and PC address below before uploading.

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ---------------- User configuration ----------------
const char *WIFI_SSID = "7538";
const char *WIFI_PASSWORD = "775751422";
const char *DEVICE_ID = "ESP32-S3-R-01";
const char *BACKEND_HOST = "192.168.76.241"; // legacy TCP fallback
constexpr uint16_t BACKEND_PORT = 8765;
const char *HTTP_TELEMETRY_URL = "https://www.u500703.nyat.app:63358/api/telemetry";
const char *HTTP_COMMANDS_URL = "https://www.u500703.nyat.app:63358/api/commands";
const char *API_TOKEN = "u500703-esp32-telemetry-2026-change-me";
constexpr bool USE_HTTP_POST = true;
constexpr bool USE_HTTPS_INSECURE_FOR_TEST = true;

constexpr uint32_t DEVICE_SERIAL_BAUD = 115200;
constexpr int SERIAL_RX_PIN = 17;
constexpr int SERIAL_TX_PIN = 18;
// Sensor output device: 9600 baud, 8 data bits, 1 stop bit, no parity.
constexpr uint32_t SENSOR_SERIAL_BAUD = 9600;
constexpr int SENSOR_RX_PIN = 4;
constexpr int SENSOR_TX_PIN = 5;
constexpr bool DEBUG_SENSOR_TEST = false;
constexpr uint32_t DEBUG_SENSOR_INTERVAL_MS = 5000;
constexpr size_t MAX_LINE_LENGTH = 768;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t TCP_RETRY_INTERVAL_MS = 3000;

HardwareSerial DeviceSerial(1);
HardwareSerial SensorSerial(2);
WiFiClient tcpClient;
String deviceLine;
String sensorLine;
String networkLine;
uint32_t nextWiFiRetry = 0;
uint32_t nextTcpRetry = 0;
uint32_t nextDebugSensor = 0;
uint32_t nextHttpCommandPoll = 0;

void sendJson(JsonDocument &document) {
  if (!tcpClient.connected()) {
    return;
  }
  serializeJson(document, tcpClient);
  tcpClient.write('\n');
}

void sendStatus(const char *state, const char *detail = nullptr) {
  JsonDocument document;
  document["type"] = "status";
  document["state"] = state;
  document["ip"] = WiFi.localIP().toString();
  if (detail != nullptr) {
    document["detail"] = detail;
  }
  sendJson(document);
}

void sendSerialLine(const String &line) {
  JsonDocument document;
  document["type"] = "serial";
  document["device_id"] = DEVICE_ID;
  document["data"] = line;
  document["millis"] = millis();
  sendJson(document);
}

void sendSensorReading(const char *module, float humidity, float temperature, const char *raw) {
  JsonDocument document;
  document["type"] = "serial";
  document["device_id"] = DEVICE_ID;
  JsonObject data = document["data"].to<JsonObject>();
  data["module"] = module;
  data["temperature"] = temperature;
  data["humidity"] = humidity;
  data["raw"] = raw;
  document["millis"] = millis();
  if (USE_HTTP_POST && WiFi.status() == WL_CONNECTED) {
    String payload;
    serializeJson(document, payload);
    WiFiClientSecure secureClient;
    if (USE_HTTPS_INSECURE_FOR_TEST) {
      secureClient.setInsecure();
    }
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(1500);
    http.begin(secureClient, HTTP_TELEMETRY_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Token", API_TOKEN);
    const int statusCode = http.POST(payload);
    Serial.printf("HTTP telemetry POST: %d\n", statusCode);
    http.end();
  } else {
    sendJson(document);
  }
}

bool parseSensorLine(const String &line, float &humidity, float &temperature) {
  // Expected device output: R:055.4RH 026.5C
  const int colon = line.indexOf(':');
  const int rh = line.indexOf("RH", colon + 1);
  const int c = line.lastIndexOf('C');
  if (colon < 0 || rh < 0 || c < 0 || rh <= colon || c <= rh) {
    return false;
  }
  String humidityText = line.substring(colon + 1, rh);
  String temperatureText = line.substring(rh + 2, c);
  humidityText.trim();
  temperatureText.trim();
  if (humidityText.isEmpty() || temperatureText.isEmpty()) {
    return false;
  }
  humidity = humidityText.toFloat();
  temperature = temperatureText.toFloat();
  return humidity >= 0.0f && humidity <= 100.0f && temperature >= -50.0f && temperature <= 150.0f;
}

void startWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  nextWiFiRetry = millis() + WIFI_RETRY_INTERVAL_MS;
}

void maintainWiFi() {
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return;
  }

  // Do not call WiFi.begin while the station is still negotiating. ESP32-S3
  // supports 2.4 GHz WiFi only; retry after a real disconnect or timeout.
  const bool retryable = status == WL_DISCONNECTED || status == WL_NO_SSID_AVAIL ||
                         status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST;
  if (retryable && static_cast<int32_t>(millis() - nextWiFiRetry) >= 0) {
    startWiFi();
  }
}

void maintainTcp() {
  if (USE_HTTP_POST) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (tcpClient.connected()) {
      tcpClient.stop();
    }
    return;
  }

  if (tcpClient.connected()) {
    return;
  }

  if (static_cast<int32_t>(millis() - nextTcpRetry) < 0) {
    return;
  }

  nextTcpRetry = millis() + TCP_RETRY_INTERVAL_MS;
  Serial.printf("Connecting to backend %s:%u\n", BACKEND_HOST, BACKEND_PORT);
  if (tcpClient.connect(BACKEND_HOST, BACKEND_PORT)) {
    Serial.println("Backend connected");
    sendStatus("connected", "tcp");
  } else {
    Serial.println("Backend connection failed");
  }
}

void pollHttpCommands() {
  if (!USE_HTTP_POST || WiFi.status() != WL_CONNECTED || static_cast<int32_t>(millis() - nextHttpCommandPoll) < 0) {
    return;
  }
  nextHttpCommandPoll = millis() + 2000;
  WiFiClientSecure secureClient;
  if (USE_HTTPS_INSECURE_FOR_TEST) {
    secureClient.setInsecure();
  }
  HTTPClient http;
  String url = String(HTTP_COMMANDS_URL) + "?device_id=" + DEVICE_ID;
  http.setConnectTimeout(1500);
  http.setTimeout(1500);
  if (!http.begin(secureClient, url)) {
    return;
  }
  http.addHeader("X-Device-Token", API_TOKEN);
  const int statusCode = http.GET();
  if (statusCode == 200) {
    JsonDocument response;
    if (deserializeJson(response, http.getString()) == DeserializationError::Ok) {
      for (JsonObject command : response["commands"].as<JsonArray>()) {
        const char *data = command["data"] | "";
        DeviceSerial.println(data);
        Serial.printf("HTTP command -> UART: %s\n", data);
      }
    }
  }
  http.end();
}

void handleDeviceSerial() {
  while (DeviceSerial.available() > 0) {
    const char ch = static_cast<char>(DeviceSerial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!deviceLine.isEmpty()) {
        sendSerialLine(deviceLine);
      }
      deviceLine = "";
      continue;
    }
    if (deviceLine.length() < MAX_LINE_LENGTH) {
      deviceLine += ch;
    } else {
      deviceLine = "";
      sendStatus("error", "device serial line too long");
    }
  }
}

void handleSensorSerial() {
  while (SensorSerial.available() > 0) {
    const char ch = static_cast<char>(SensorSerial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!sensorLine.isEmpty()) {
        float humidity = 0.0f;
        float temperature = 0.0f;
        if (parseSensorLine(sensorLine, humidity, temperature)) {
          Serial.printf("Sensor reading: RH %.1f%%, %.1fC\n", humidity, temperature);
          sendSensorReading("R", humidity, temperature, sensorLine.c_str());
        } else {
          Serial.printf("Unrecognized sensor line: %s\n", sensorLine.c_str());
          sendSerialLine(sensorLine);
        }
      }
      sensorLine = "";
      continue;
    }
    if (sensorLine.length() < MAX_LINE_LENGTH) {
      sensorLine += ch;
    } else {
      sensorLine = "";
      sendStatus("error", "sensor serial line too long");
    }
  }
}

void emitDebugSensorReading() {
  if (!DEBUG_SENSOR_TEST || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (static_cast<int32_t>(millis() - nextDebugSensor) < 0) {
    return;
  }
  nextDebugSensor = millis() + DEBUG_SENSOR_INTERVAL_MS;
  const float phase = static_cast<float>((millis() / DEBUG_SENSOR_INTERVAL_MS) % 12);
  const float humidity = 55.4f + phase * 0.2f;
  const float temperature = 26.5f + phase * 0.1f;
  SensorSerial.println("R:055.4RH 026.5C");
  sendSensorReading("debug", humidity, temperature, "R:055.4RH 026.5C");
  Serial.printf("Debug sensor reading: RH %.1f%%, %.1fC\n", humidity, temperature);
}

void handleNetworkLine(const String &line) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, line);
  if (error) {
    sendStatus("error", "invalid JSON from backend");
    return;
  }

  const char *type = document["type"] | "";
  if (strcmp(type, "command") != 0) {
    return;
  }

  const char *data = document["data"] | "";
  DeviceSerial.println(data);
  Serial.printf("Backend command -> UART: %s\n", data);
}

void handleNetwork() {
  if (!tcpClient.connected()) {
    return;
  }

  while (tcpClient.available() > 0) {
    const char ch = static_cast<char>(tcpClient.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!networkLine.isEmpty()) {
        handleNetworkLine(networkLine);
      }
      networkLine = "";
      continue;
    }
    if (networkLine.length() < MAX_LINE_LENGTH) {
      networkLine += ch;
    } else {
      networkLine = "";
      sendStatus("error", "backend line too long");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  DeviceSerial.begin(DEVICE_SERIAL_BAUD, SERIAL_8N1, SERIAL_RX_PIN, SERIAL_TX_PIN);
  SensorSerial.begin(SENSOR_SERIAL_BAUD, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);
  Serial.println("ESP32-S3 WiFi serial bridge starting");
  Serial.printf("Sensor UART: GPIO%d RX/GPIO%d TX @ %lu 8N1\n", SENSOR_RX_PIN, SENSOR_TX_PIN, SENSOR_SERIAL_BAUD);
  startWiFi();
}

void loop() {
  maintainWiFi();
  maintainTcp();
  handleDeviceSerial();
  handleSensorSerial();
  emitDebugSensorReading();
  handleNetwork();
  pollHttpCommands();
  delay(2);
}
