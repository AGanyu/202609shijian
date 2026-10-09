// ESP32-S3 WiFi gateway.
// SensorSerial reads the local sensor; DeviceSerial receives JSON frames from
// a local sensor node and forwards both sources to the public backend.

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ---------------- User configuration ----------------
const char *WIFI_SSID = "Redmi K60_5pPpkEG_MI";
const char *WIFI_PASSWORD = "Mf6Yf3B7ZE";
const char *DEVICE_ID = "ESP32-S3-R-01";
const char *BACKEND_HOST = "192.168.76.241"; // legacy TCP fallback
constexpr uint16_t BACKEND_PORT = 8765;
const char *HTTP_TELEMETRY_URL = "https://www.u500703.nyat.app:63358/api/telemetry";
const char *HTTP_COMMANDS_URL = "https://www.u500703.nyat.app:63358/api/commands";
const char *API_TOKEN = "u500703-esp32-telemetry-2026-change-me";
constexpr bool USE_HTTP_POST = true;
constexpr bool USE_HTTPS_INSECURE_FOR_TEST = true;

constexpr uint32_t DEVICE_SERIAL_BAUD = 115200;
constexpr int NODE_RX_PIN = 15;
constexpr int NODE_TX_PIN = 16;
// Sensor output device: 9600 baud, 8 data bits, 1 stop bit, no parity.
constexpr uint32_t SENSOR_SERIAL_BAUD = 9600;
constexpr int SENSOR_RX_PIN = 4;
constexpr int SENSOR_TX_PIN = 5;
constexpr bool DEBUG_SENSOR_TEST = false;
constexpr bool DEBUG_NODE_DIAGNOSTICS = false;
constexpr bool DEBUG_USB_MIRROR = true;
constexpr uint32_t DEBUG_SENSOR_INTERVAL_MS = 5000;
// Drain the sensor UART continuously, but accept at most one valid frame per
// interval. This prevents a talkative sensor from filling the batch buffer.
constexpr uint32_t SENSOR_SAMPLE_INTERVAL_MS = 5000;
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
uint32_t nextSensorRead = 0;
uint32_t nextHttpCommandPoll = 0;

constexpr uint16_t BATCH_INTERVAL_MS = 5000;
constexpr uint16_t BATCH_MAX_MESSAGES = 64;

struct BatchBuffer {
  uint16_t count;
  char messages[BATCH_MAX_MESSAGES][MAX_LINE_LENGTH];
};

BatchBuffer batchA{};
BatchBuffer batchB{};
BatchBuffer *activeBatch = &batchA;
BatchBuffer *sendingBatch = &batchB;
SemaphoreHandle_t batchMutex = nullptr;
void pollHttpCommands();

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

bool postTelemetry(JsonDocument &document) {
  if (!USE_HTTP_POST || WiFi.status() != WL_CONNECTED) {
    sendJson(document);
    return false;
  }

  String payload;
  serializeJson(document, payload);
  WiFiClientSecure secureClient;
  if (USE_HTTPS_INSECURE_FOR_TEST) {
    secureClient.setInsecure();
  }
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(1500);
  if (!http.begin(secureClient, HTTP_TELEMETRY_URL)) {
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", API_TOKEN);
  const int statusCode = http.POST(payload);
  Serial.printf("HTTP telemetry POST: %d\n", statusCode);
  if (statusCode >= 400) {
    String responseBody = http.getString();
    Serial.printf("[HTTP ERROR BODY] %s\n", responseBody.c_str());
  }
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

void queueBatchMessage(const String &line) {
  if (batchMutex == nullptr || line.isEmpty()) {
    return;
  }
  if (xSemaphoreTake(batchMutex, 0) == pdTRUE) {
    if (activeBatch->count < BATCH_MAX_MESSAGES) {
      snprintf(activeBatch->messages[activeBatch->count], MAX_LINE_LENGTH, "%s", line.c_str());
      activeBatch->count++;
    }
    xSemaphoreGive(batchMutex);
  }
}

String makeGatewaySensorMessage(const String &raw) {
  JsonDocument document;
  document["module"] = "R";
  document["id"] = DEVICE_ID;
  document["raw"] = raw;
  String payload;
  serializeJson(document, payload);
  return payload;
}

bool postBatch(BatchBuffer &batch) {
  if (batch.count == 0) {
    return true;
  }
  JsonDocument document;
  document["type"] = "batch";
  document["device_id"] = DEVICE_ID;
  document["interval_ms"] = BATCH_INTERVAL_MS;
  JsonArray messages = document["messages"].to<JsonArray>();
  for (uint16_t i = 0; i < batch.count; ++i) {
    messages.add(batch.messages[i]);
  }
  return postTelemetry(document);
}

void restoreBatch(BatchBuffer &batch) {
  if (batch.count == 0 || batchMutex == nullptr) {
    return;
  }
  if (xSemaphoreTake(batchMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (uint16_t i = 0; i < batch.count && activeBatch->count < BATCH_MAX_MESSAGES; ++i) {
      snprintf(activeBatch->messages[activeBatch->count], MAX_LINE_LENGTH, "%s", batch.messages[i]);
      activeBatch->count++;
    }
    xSemaphoreGive(batchMutex);
  }
}

void batchWorker(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(BATCH_INTERVAL_MS));
    if (batchMutex == nullptr || xSemaphoreTake(batchMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
      continue;
    }
    BatchBuffer *ready = activeBatch;
    activeBatch = sendingBatch;
    sendingBatch = ready;
    activeBatch->count = 0;
    xSemaphoreGive(batchMutex);
    const bool posted = postBatch(*sendingBatch);
    Serial.printf("[BATCH] count=%u result=%s\n", sendingBatch->count, posted ? "OK" : "FAILED");
    if (!posted) {
      restoreBatch(*sendingBatch);
    }
    sendingBatch->count = 0;
  }
}

void commandWorker(void *) {
  for (;;) {
    pollHttpCommands();
    vTaskDelay(pdMS_TO_TICKS(20));
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
    const uint8_t byteValue = static_cast<uint8_t>(DeviceSerial.read());
    const char ch = static_cast<char>(byteValue);
    // Transparent bridge: every byte from GPIO15/RX is returned to the PC.
    Serial.write(byteValue);
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!deviceLine.isEmpty()) {
        queueBatchMessage(deviceLine);
        if (DEBUG_USB_MIRROR) {
          Serial.printf("[GPIO15 RX] %s\n", deviceLine.c_str());
        }
        if (DEBUG_NODE_DIAGNOSTICS) {
          Serial.printf("[DEBUG-GATEWAY] node frame: %s\n", deviceLine.c_str());
        }
        JsonDocument incoming;
        const DeserializationError error = deserializeJson(incoming, deviceLine);
        if (error || !incoming.is<JsonObject>()) {
          if (DEBUG_NODE_DIAGNOSTICS) {
            Serial.printf("[DEBUG-GATEWAY] JSON parse failed: %s\n", error.c_str());
          }
          sendStatus("error", "invalid local node JSON");
        } else {
          const char *nodeIdValue = incoming["node_id"] | "";
          const char *moduleValue = incoming["module"] | "R";
          const char *rawValue = incoming["raw"] | "";
          const bool hasTemperature = !incoming["temperature"].isNull();
          const bool hasHumidity = !incoming["humidity"].isNull();
          const float temperature = incoming["temperature"] | 0.0f;
          const float humidity = incoming["humidity"] | 0.0f;
          if (nodeIdValue[0] == '\0' || !hasTemperature || !hasHumidity ||
              humidity < 0.0f || humidity > 100.0f || temperature < -50.0f ||
              temperature > 150.0f) {
            if (DEBUG_NODE_DIAGNOSTICS) {
              Serial.println("[DEBUG-GATEWAY] node reading validation failed");
            }
            sendStatus("error", "invalid local node reading");
          } else {
            // The complete JSON frame is already in the 5-second batch.
          }
        }
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

void handleUsbSerial() {
  while (Serial.available() > 0) {
    const uint8_t byteValue = static_cast<uint8_t>(Serial.read());
    // Transparent bridge: every byte from the PC is sent to GPIO16/TX.
    DeviceSerial.write(byteValue);
  }
}

void handleSensorSerial() {
  while (SensorSerial.available() > 0) {
    const uint8_t byteValue = static_cast<uint8_t>(SensorSerial.read());
    const char ch = static_cast<char>(byteValue);
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!sensorLine.isEmpty()) {
        // Always consume complete lines so the UART cannot back up. During
        // the cooldown, discard them without parsing or network work.
        if (static_cast<int32_t>(millis() - nextSensorRead) >= 0) {
          float humidity = 0.0f;
          float temperature = 0.0f;
          if (parseSensorLine(sensorLine, humidity, temperature)) {
            queueBatchMessage(makeGatewaySensorMessage(sensorLine));
            nextSensorRead = millis() + SENSOR_SAMPLE_INTERVAL_MS;
            Serial.printf("[GPIO4 RX] %s | RH %.1f%%, %.1fC\n", sensorLine.c_str(), humidity, temperature);
          }
        }
      }
      sensorLine = "";
      continue;
    }
    // Sensor frames are ASCII. Drop startup noise and malformed high-bit
    // bytes so they cannot make the enclosing HTTPS JSON invalid UTF-8.
    if (byteValue < 0x20 || byteValue > 0x7e) {
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
  queueBatchMessage(makeGatewaySensorMessage("R:055.4RH 026.5C"));
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
  DeviceSerial.begin(DEVICE_SERIAL_BAUD, SERIAL_8N1, NODE_RX_PIN, NODE_TX_PIN);
  SensorSerial.begin(SENSOR_SERIAL_BAUD, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);
  Serial.println("ESP32-S3 WiFi gateway starting");
  Serial.printf("Local node UART: GPIO%d RX/GPIO%d TX @ %lu 8N1\n", NODE_RX_PIN, NODE_TX_PIN, DEVICE_SERIAL_BAUD);
  Serial.printf("Sensor UART: GPIO%d RX/GPIO%d TX @ %lu 8N1\n", SENSOR_RX_PIN, SENSOR_TX_PIN, SENSOR_SERIAL_BAUD);
  batchMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(batchWorker, "batch", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(commandWorker, "commands", 6144, nullptr, 1, nullptr, 0);
  startWiFi();
}

void loop() {
  // GPIO15/16 is the highest-priority path. Drain both UART directions first.
  handleDeviceSerial();
  handleUsbSerial();
  maintainWiFi();
  maintainTcp();
  handleSensorSerial();
  emitDebugSensorReading();
  handleNetwork();
  delay(0);
}
