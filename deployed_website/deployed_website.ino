#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>

// Pin Definitions
#define DS18B20_PIN 15
#define RELAY_PIN 5
// #define RELAY_PIN 14
#define LED_PIN 2
#define TX_PIN 16
#define RX_PIN 17

// Default WiFi Credentials
char ssid[32] = "HANJEE FIBER - SAAD";
char password[32] = "lahore1222";

// MQTT Broker Settings
const char* mqtt_server = "192.168.100.36";  // Your server IP
const int mqtt_port = 1883;

// MQTT Topics
const char* MQTT_TOPIC_SENSOR_DATA = "device/data";
const char* MQTT_TOPIC_SETTINGS = "device/settings";

// Initialize sensors
OneWire ourWire(DS18B20_PIN);
DallasTemperature sensor(&ourWire);
PZEM004Tv30 pzem1(Serial2, RX_PIN, TX_PIN);

// MQTT Client
WiFiClient espClient;
PubSubClient client(espClient);

// Settings Variables
float temperatureThreshold = 25.0;
bool reverseRelay = false;
bool relayState = false;

// Device ID (MAC Address)
String deviceId;

// Helper function to safely handle NaN
float safeValue(float value) {
  return isnan(value) ? 0.0 : value;
}

String getMACAddress() {
  if (deviceId.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    deviceId = String(macStr);
  }
  return deviceId;
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    String clientId = "ESP32_" + getMACAddress();

    if (client.connect(clientId.c_str())) {
      Serial.println("Connected to MQTT broker");

      // Subscribe to settings topic
      client.subscribe(MQTT_TOPIC_SETTINGS);

      // Publish initial state
      publishSensorData(sensor.getTempCByIndex(0),
                        pzem1.voltage(),
                        pzem1.current(),
                        pzem1.power());
    } else {
      Serial.print("Failed, rc=");
      Serial.print(client.state());
      Serial.println(" Retrying in 5 seconds...");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  StaticJsonDocument<300> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (!error) {
    const char* targetDeviceId = doc["deviceId"];
    if (targetDeviceId && String(targetDeviceId) == getMACAddress()) {
      bool wifiCredentialsUpdated = false;

      if (strcmp(topic, MQTT_TOPIC_SETTINGS) == 0) {
        if (doc.containsKey("threshold")) {
          temperatureThreshold = doc["threshold"];
          Serial.printf("Updated temperature threshold: %.2f°C\n", temperatureThreshold);
        }

        if (doc.containsKey("reverseRelay")) {
          reverseRelay = doc["reverseRelay"];
          Serial.printf("Updated reverseRelay: %s\n", reverseRelay ? "true" : "false");
        }

        if (doc.containsKey("ssid")) {
          String newSsid = doc["ssid"];
          if (newSsid != ssid) {  // Check if SSID is actually changed
            strlcpy(ssid, newSsid.c_str(), sizeof(ssid));
            Serial.printf("Updated WiFi SSID: %s\n", ssid);
            wifiCredentialsUpdated = true;
          }
        }

        if (doc.containsKey("password")) {
          String newPassword = doc["password"];
          if (newPassword != password) {  // Check if password is actually changed
            strlcpy(password, newPassword.c_str(), sizeof(password));
            Serial.println("Updated WiFi Password");
            wifiCredentialsUpdated = true;
          }
        }

        // Reconnect to WiFi if credentials were updated
        if (wifiCredentialsUpdated) {
          Serial.println("WiFi credentials updated. Reconnecting to WiFi...");
          WiFi.disconnect();
          WiFi.begin(ssid, password);

          unsigned long startAttemptTime = millis();

          // Wait for connection (with a timeout of 10 seconds)
          while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
            delay(500);
            Serial.print(".");
          }

          if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nWiFi reconnected successfully");
            Serial.print("New IP address: ");
            Serial.println(WiFi.localIP());
          } else {
            Serial.println("\nFailed to reconnect to WiFi");
          }
        }
      }
    }
  }
}


void updateRelayState(float temperature) {
  bool shouldRelayBeOn = reverseRelay ? (temperature < temperatureThreshold) : (temperature >= temperatureThreshold);
  if (shouldRelayBeOn != relayState) {
    relayState = shouldRelayBeOn;
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
    digitalWrite(LED_PIN, relayState ? HIGH : LOW);
  }
}

void publishSensorData(float tempC, float voltage, float current, float power) {
  if (!client.connected()) {
    reconnectMQTT();
  }

  StaticJsonDocument<350> doc;

  // Add device identification
  doc["deviceId"] = getMACAddress();

  // Sensor readings
  doc["temperature"] = safeValue(tempC);
  doc["voltage"] = safeValue(voltage);
  doc["current"] = safeValue(current);
  doc["power"] = safeValue(power);

  // Device state
  doc["relayState"] = relayState;
  doc["temperatureThreshold"] = temperatureThreshold;

  // Add timestamp
  doc["timestamp"] = millis();

  char jsonBuffer[350];
  serializeJson(doc, jsonBuffer);

  client.publish(MQTT_TOPIC_SENSOR_DATA, jsonBuffer);
  Serial.println("Published to MQTT:");
  Serial.println(jsonBuffer);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // Initialize Serial2 for PZEM
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  // Initialize temperature sensor
  sensor.begin();

  // Connect to WiFi
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  // Read sensors
  sensor.requestTemperatures();
  float tempC = sensor.getTempCByIndex(0);
  float voltage = safeValue(pzem1.voltage());
  float current = safeValue(pzem1.current());
  float power = safeValue(pzem1.power());

  // Update relay state based on temperature
  updateRelayState(tempC);

  // Publish data to MQTT
  publishSensorData(tempC, voltage, current, power);

  delay(5000);  // Wait 5 seconds before next reading
}