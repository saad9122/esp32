#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PZEM004Tv30.h>

// Pin Definitions
#define DS18B20_PIN 15
#define RELAY_PIN 5
#define LED_PIN 2
#define TX_PIN 16
#define RX_PIN 17

// WiFi Credentials
const char* ssid = "Infinix";
const char* password = "lahore1222";

// Local Server URLs
const char* sensorDataURL = "https://iot-rust-gamma.vercel.app/api/sensors";
const char* thresholdURL = "https://iot-rust-gamma.vercel.app/api/threshold";

// Initialize sensors
OneWire ourWire(DS18B20_PIN);
DallasTemperature sensor(&ourWire);
PZEM004Tv30 pzem1(Serial2, RX_PIN, TX_PIN);

// Dynamic Temperature Threshold
float temperatureThreshold = 25.0;
bool relayState = false;

// Helper function to safely handle NaN and convert to 0
float safeValue(float value) {
  return isnan(value) ? 0.0 : value;
}

// Function to fetch the threshold value from the server
void fetchThresholdValue() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected. Cannot fetch threshold.");
    return;
  }

  HTTPClient http;
  http.begin(thresholdURL);

  int httpResponseCode = http.GET();

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.print("Threshold Response: ");
    Serial.println(response);

    // Parse the response and update the threshold value
    float newThreshold = response.toFloat();
    if (newThreshold > 0) {  // Add validation if necessary
      temperatureThreshold = newThreshold;
      Serial.printf("Updated Temperature Threshold: %.2f\n", temperatureThreshold);
    } else {
      Serial.println("Invalid threshold value received.");
    }
  } else {
    Serial.print("Error fetching threshold. HTTP Response Code: ");
    Serial.println(httpResponseCode);
  }

  http.end();
}

void sendDataToServer(float tempC, float voltage, float current, float power) {
  // Sanitize values
  tempC = safeValue(tempC);
  voltage = safeValue(voltage);
  current = safeValue(current);
  power = safeValue(power);

  // Debugging output
  Serial.println("------- Sending Sensor Data -------");
  Serial.printf("Temperature: %.2f°C\n", tempC);
  Serial.printf("Voltage: %.2f V\n", voltage);
  Serial.printf("Current: %.2f A\n", current);
  Serial.printf("Power: %.2f W\n", power);

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nReconnected to WiFi");
  }


  HTTPClient http;
  http.begin(sensorDataURL);
  http.addHeader("Content-Type", "application/json");

  // Create JSON payload with careful formatting
  char jsonPayload[250];
  snprintf(jsonPayload, sizeof(jsonPayload),
           "{\"temperature\":%.2f,\"voltage\":%.2f,\"current\":%.2f,\"power\":%.2f,\"relayState\":%s,\"temperatureThreshold\":%.2f}",
           tempC, voltage, current, power,
           relayState ? "true" : "false",
           temperatureThreshold);

  Serial.print("JSON Payload: ");
  Serial.println(jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Server Response:");
    Serial.println(response);
  } else {
    Serial.print("Error sending data. HTTP Response Code: ");
    Serial.println(httpResponseCode);
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // WiFi Connection
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize sensors and pins
  sensor.begin();
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
}

void loop() {
  // Read temperature
  sensor.requestTemperatures();
  float tempC = sensor.getTempCByIndex(0);

  // Read PZEM004T data with error handling
  float voltage = safeValue(pzem1.voltage());
  float current = safeValue(pzem1.current());
  float power = safeValue(pzem1.power());

  // Control relay based on temperature
  if (tempC >= temperatureThreshold && relayState) {
    digitalWrite(RELAY_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    relayState = false;
  } else if (tempC < temperatureThreshold && !relayState) {
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH);
    relayState = true;
  }

  // Send data to server
  sendDataToServer(tempC, voltage, current, power);

  // Fetch updated threshold value
  fetchThresholdValue();

  delay(5000);  // Execute loop every 5 seconds
}
