/*
 * VitalTrack - ESP32-S3 firmware
 *
 * Samples heart rate, SpO2, body temperature and movement, and publishes
 * them as retained MQTT messages under health/.
 *
 * Author: Senator Chigoziri Ugwu
 * B.Eng. Computer Engineering, Nile University of Nigeria
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// WiFi and broker settings live in secrets.h, which is git-ignored.
// Copy secrets.example.h to secrets.h and fill in your own values.
#include "secrets.h"

const int   MQTT_PORT = 1883;

#define I2C_SDA      8
#define I2C_SCL      9
#define PULSE_PIN    4
#define DS18B20_PIN  14

#define WIFI_CONNECT_TIMEOUT_MS  15000UL
#define MQTT_RETRY_INTERVAL_MS    5000UL
#define MAX_BLOCK_MS              3000UL
#define MAX_SAMPLE_TIMEOUT_MS      500UL

#define INTERVAL_PULSE      200UL
#define INTERVAL_MAX       5000UL
#define INTERVAL_TEMP      5000UL
#define INTERVAL_MOVEMENT   500UL

OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);
WiFiClient        wifi;
PubSubClient      mqtt(wifi);
MAX30105          particleSensor;
Adafruit_MPU6050  mpu;

uint32_t irBuffer[100];
uint32_t redBuffer[100];
int32_t  spo2Val;
int8_t   spo2Valid;
int32_t  heartRateVal;
int8_t   hrValid;

int   lastBpm      = 0;
float lastAccelMag = 0.0f;

unsigned long lastPulse    = 0;
unsigned long lastMAX      = 0;
unsigned long lastMovement = 0;
unsigned long lastTemp     = 0;
unsigned long lastMqttTry  = 0;

bool maxReady  = false;
bool mpuReady  = false;
bool tempReady = false;

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\n[WiFi] Timed out — continuing offline");
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
}

void handleMqtt() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastMqttTry < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttTry = now;
  Serial.print("[MQTT] Connecting...");
  if (mqtt.connect("esp32-health")) {
    Serial.println(" Connected!");
  } else {
    Serial.printf(" Failed rc=%d — retrying in %lu s\n",
                  mqtt.state(), MQTT_RETRY_INTERVAL_MS / 1000);
  }
}

void publishInt(const char* topic, int val) {
  if (!mqtt.connected()) return;
  char buf[12];
  itoa(val, buf, 10);
  mqtt.publish(topic, buf, true);
}

void publishFloat(const char* topic, float val, int decimals) {
  if (!mqtt.connected()) return;
  char buf[16];
  dtostrf(val, 4, decimals, buf);
  mqtt.publish(topic, buf, true);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);   // wait for Serial Monitor to connect
  delay(2000);
  Serial.println("\n=== Booting ===");

  // Clear stuck I2C bus before init
  pinMode(I2C_SDA, OUTPUT);
  pinMode(I2C_SCL, OUTPUT);
  digitalWrite(I2C_SDA, HIGH);
  digitalWrite(I2C_SCL, HIGH);
  for (int i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
  }
  digitalWrite(I2C_SDA, LOW); delayMicroseconds(5);
  digitalWrite(I2C_SDA, HIGH); delayMicroseconds(5);

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("[I2C] SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);

  connectWifi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);

  // MAX30102
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[MAX30102] Not found — SKIPPING");
  } else {
    particleSensor.setup(60, 4, 2, 100, 411, 4096);
    Serial.println("[MAX30102] Ready");
    maxReady = true;
  }

  // MPU6050
  if (!mpu.begin()) {
    Serial.println("[MPU6050] Not found — SKIPPING");
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
    mpu.setGyroRange(MPU6050_RANGE_250_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("[MPU6050] Ready");
    mpuReady = true;
  }

  // DS18B20
  tempSensor.begin();
  int count = tempSensor.getDeviceCount();
  Serial.printf("[DS18B20] Found %d sensor(s)\n", count);
  if (count > 0) tempReady = true;

  Serial.println("=== All done — running! ===\n");
}

void loop() {
  connectWifi();
  handleMqtt();

  unsigned long now = millis();

  // Pulse sensor every 200ms
  if (now - lastPulse >= INTERVAL_PULSE) {
    lastPulse = now;
    int raw = analogRead(PULSE_PIN);
    int bpm = map(raw, 0, 4095, 40, 150);
    bpm = (bpm + lastBpm) / 2;
    lastBpm = bpm;
    if (bpm > 45 && bpm < 145) {
      publishInt("health/heartrate", bpm);
      Serial.printf("[Pulse] raw=%d BPM=%d\n", raw, bpm);
    }
  }

  // MAX30102 SpO2 every 5s
  if (maxReady && now - lastMAX >= INTERVAL_MAX) {
    lastMAX = now;
    unsigned long blockStart = millis();
    int samplesCollected = 0;
    for (int i = 0; i < 100; i++) {
      if (millis() - blockStart > MAX_BLOCK_MS) {
        Serial.printf("[MAX30102] Block timeout after %d samples\n", i);
        break;
      }
      unsigned long t = millis();
      while (!particleSensor.available()) {
        particleSensor.check();
        if (millis() - t > MAX_SAMPLE_TIMEOUT_MS) {
          Serial.println("[MAX30102] Sample timeout");
          goto max_done;
        }
      }
      redBuffer[i] = particleSensor.getRed();
      irBuffer[i]  = particleSensor.getIR();
      particleSensor.nextSample();
      samplesCollected++;
    }
    max_done:
    if (samplesCollected >= 50) {
      maxim_heart_rate_and_oxygen_saturation(
        irBuffer, samplesCollected, redBuffer,
        &spo2Val, &spo2Valid, &heartRateVal, &hrValid
      );
      if (spo2Valid == 1 && spo2Val > 0 && spo2Val <= 100) {
        publishInt("health/spo2", spo2Val);
        Serial.printf("[MAX30102] SpO2=%d%%\n", spo2Val);
      }
      if (hrValid == 1 && heartRateVal > 30 && heartRateVal < 200) {
        publishInt("health/heartrate", heartRateVal);
        Serial.printf("[MAX30102] HR=%d BPM\n", heartRateVal);
      }
    } else {
      Serial.printf("[MAX30102] Too few samples (%d) — skipping\n", samplesCollected);
    }
  }

  // DS18B20 temperature every 5s
  if (tempReady && now - lastTemp >= INTERVAL_TEMP) {
    lastTemp = now;
    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);
    if (tempC != DEVICE_DISCONNECTED_C && tempC > 0.0f) {
      publishFloat("health/temperature", tempC, 1);
      Serial.printf("[DS18B20] Temp=%.1f C\n", tempC);
    } else {
      Serial.println("[DS18B20] No reading");
    }
  }

  // MPU6050 movement every 500ms
  if (mpuReady && now - lastMovement >= INTERVAL_MOVEMENT) {
    lastMovement = now;
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float ax   = a.acceleration.x;
    float ay   = a.acceleration.y;
    float az   = a.acceleration.z;
    float mag  = sqrtf(ax*ax + ay*ay + az*az);
    float diff = fabsf(mag - lastAccelMag);
    lastAccelMag = mag;
    bool moving  = (diff > 0.3f);
    publishInt("health/movement", moving ? 1 : 0);
    Serial.printf("[MPU6050] mag=%.2f diff=%.3f moving=%d\n", mag, diff, (int)moving);
  }

  delay(10);
}