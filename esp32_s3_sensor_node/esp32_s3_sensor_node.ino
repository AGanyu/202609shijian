// ESP32-S3 local sensor node.
// Reads the same 9600 8N1 sensor format as the gateway and sends one JSON
// frame per reading over a separate 115200 8N1 UART.

#include <ArduinoJson.h>

const char *NODE_ID = "ESP32-S3-NODE-01";
constexpr bool DEBUG_DIAGNOSTICS = true;

constexpr uint32_t SENSOR_SERIAL_BAUD = 9600;
constexpr int SENSOR_RX_PIN = 4;
constexpr int SENSOR_TX_PIN = 5;
constexpr uint32_t GATEWAY_SERIAL_BAUD = 115200;
constexpr int GATEWAY_RX_PIN = 15;
constexpr int GATEWAY_TX_PIN = 16;
constexpr size_t MAX_LINE_LENGTH = 256;

HardwareSerial SensorSerial(2);
HardwareSerial GatewaySerial(1);
String sensorLine;

bool parseSensorLine(const String &line, float &humidity, float &temperature) {
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
  return humidity >= 0.0f && humidity <= 100.0f && temperature >= -50.0f &&
         temperature <= 150.0f;
}

void sendReading(const String &raw, float humidity, float temperature) {
  JsonDocument document;
  document["node_id"] = NODE_ID;
  document["module"] = "B";
  document["temperature"] = temperature;
  document["humidity"] = humidity;
  document["raw"] = raw;
  String payload;
  serializeJson(document, payload);
  if (DEBUG_DIAGNOSTICS) {
    Serial.printf("[DEBUG-NODE] sending JSON: %s\n", payload.c_str());
  }
  serializeJson(document, GatewaySerial);
  GatewaySerial.println();
}

void handleSensorSerial() {
  while (SensorSerial.available() > 0) {
    const char ch = static_cast<char>(SensorSerial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!sensorLine.isEmpty()) {
        if (DEBUG_DIAGNOSTICS) {
          Serial.printf("[DEBUG-NODE] sensor frame: %s\n", sensorLine.c_str());
        }
        float humidity = 0.0f;
        float temperature = 0.0f;
        if (parseSensorLine(sensorLine, humidity, temperature)) {
          sendReading(sensorLine, humidity, temperature);
        } else if (DEBUG_DIAGNOSTICS) {
          Serial.println("[DEBUG-NODE] sensor parse failed");
        }
      }
      sensorLine = "";
      continue;
    }
    if (sensorLine.length() < MAX_LINE_LENGTH) {
      sensorLine += ch;
    } else {
      sensorLine = "";
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  if (DEBUG_DIAGNOSTICS) {
    Serial.println("[DEBUG-NODE] boot");
    Serial.printf("[DEBUG-NODE] sensor UART RX=%d TX=%d @ %lu\n", SENSOR_RX_PIN,
                  SENSOR_TX_PIN, SENSOR_SERIAL_BAUD);
    Serial.printf("[DEBUG-NODE] gateway UART TX=%d RX=%d @ %lu, module=B\n",
                  GATEWAY_TX_PIN, GATEWAY_RX_PIN, GATEWAY_SERIAL_BAUD);
  }
  SensorSerial.begin(SENSOR_SERIAL_BAUD, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);
  GatewaySerial.begin(GATEWAY_SERIAL_BAUD, SERIAL_8N1, GATEWAY_RX_PIN, GATEWAY_TX_PIN);
}

void loop() {
  handleSensorSerial();
  delay(2);
}
