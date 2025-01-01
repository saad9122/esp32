#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// Pin Definitions
#define DS18B20_PIN 15
#define RELAY_PIN 5
#define LED_PIN 2
#define TX_PIN 16
#define RX_PIN 17
#define CONFIG_RESET_PIN 4  // Pin to reset WiFi settings

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
WiFiManager wifiManager;

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
    // Ensure WiFi is started before fetching MAC address
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin();  // Start WiFi if not already connected
      unsigned long startAttemptTime = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        delay(100);
      }
    }

    // Fetch the MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    deviceId = String(macStr);
  }
  return deviceId;
}

void saveConfigCallback() {
  Serial.println("Should save config");
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    String clientId = "ESP32_" + getMACAddress();

    if (client.connect(clientId.c_str())) {
      Serial.println("Connected to MQTT broker");
      client.subscribe(MQTT_TOPIC_SETTINGS);
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
      if (strcmp(topic, MQTT_TOPIC_SETTINGS) == 0) {
        if (doc.containsKey("threshold")) {
          temperatureThreshold = doc["threshold"];
          Serial.printf("Updated temperature threshold: %.2f°C\n", temperatureThreshold);
        }

        if (doc.containsKey("reverseRelay")) {
          reverseRelay = doc["reverseRelay"];
          Serial.printf("Updated reverseRelay: %s\n", reverseRelay ? "true" : "false");
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

  doc["deviceId"] = getMACAddress();
  doc["temperature"] = safeValue(tempC);
  doc["voltage"] = safeValue(voltage);
  doc["current"] = safeValue(current);
  doc["power"] = safeValue(power);
  doc["relayState"] = relayState;
  doc["temperatureThreshold"] = temperatureThreshold;
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
  pinMode(CONFIG_RESET_PIN, INPUT_PULLUP);

  // Initialize Serial2 for PZEM
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  // Initialize temperature sensor
  sensor.begin();

  // Set WiFiManager callbacks
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setAPCallback(configModeCallback);

  // Create unique device name for AP mode
  String apName = "ESP32_" + getMACAddress();
  
  // Configure timeout
  wifiManager.setConfigPortalTimeout(180); // 3 minutes timeout
  
  // Enable Debug mode
  wifiManager.setDebugOutput(true);

  // Check if need to reset settings
  if (digitalRead(CONFIG_RESET_PIN) == LOW) {
    Serial.println("Reset button pressed, resetting WiFi settings");
    wifiManager.resetSettings();
    ESP.restart();
  }

  // Try to connect to saved WiFi or start config portal
  if (!wifiManager.autoConnect(apName.c_str())) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Connected to WiFi");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  // Check if WiFi is connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost. Attempting to reconnect...");
    wifiManager.autoConnect();
  }

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