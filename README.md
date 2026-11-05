# VitalTrack

An IoT patient-monitoring system that tracks heart rate, SpO2, body temperature and movement in
real time, and streams them to a live web dashboard.

Final year project, B.Eng. Computer Engineering, Nile University of Nigeria (2026).

---

## How it works

```
   ESP32-S3                    Raspberry Pi 4                    Browser
 ┌────────────┐              ┌─────────────────┐            ┌──────────────┐
 │ MAX30105   │  heart/SpO2  │                 │            │              │
 │ DS18B20    │──────────────│  Mosquitto MQTT │            │  Dashboard   │
 │ MPU6050    │    MQTT      │       ↓         │  WebSocket │  live cards  │
 │ pulse pin  │   health/#   │  Flask backend  │────────────│  Chart.js    │
 └────────────┘              │  + alert logic  │            │  alerts      │
                             └─────────────────┘            └──────────────┘
```

The ESP32-S3 samples each sensor on its own interval and publishes retained MQTT messages under
`health/`. The Raspberry Pi runs the broker and a Flask backend that subscribes to `health/#`,
holds current state plus a rolling 120-point history, evaluates alert thresholds, and pushes
updates to the browser over WebSocket. The dashboard renders live sensor cards and charts.

| Measurement | Sensor | Sample interval |
|---|---|---|
| Heart rate, SpO2 | MAX30105 pulse oximeter | 5 s |
| Heart rate (pulse pin) | Analogue pulse sensor | 200 ms |
| Body temperature | DS18B20 | 5 s |
| Movement | MPU6050 accelerometer/gyro | 500 ms |

### Alert thresholds

| Condition | Threshold |
|---|---|
| Temperature | below 35.0 °C or above 38.5 °C |
| Heart rate | below 50 or above 120 BPM |
| SpO2 | below 95% |
| Inactivity | no movement for 1 hour |

---

## Measured performance

The system was validated against reference instruments rather than assumed to work:

| Metric | Result |
|---|---|
| Mean temperature error | 0.063 °C |
| Mean heart-rate error | 2.0 BPM |
| Alert response time | 4.3 – 6.5 s |
| Uptime over 24 hours | 98.7% |

---

## Repository layout

```
firmware/healthcode/     ESP32-S3 firmware (C++/Arduino)
  healthcode.ino         Sensor sampling, WiFi/MQTT, non-blocking main loop
  secrets.example.h      Template for WiFi and broker settings
backend/server.py        Raspberry Pi: MQTT subscriber, alert logic, WebSocket server
dashboard/index.html     Live dashboard — sensor cards, Chart.js history, alert banner
```

---

## Running it

### Firmware

Copy the secrets template and fill in your own values:

```bash
cp firmware/healthcode/secrets.example.h firmware/healthcode/secrets.h
```

Open `healthcode.ino` in the Arduino IDE, select an ESP32-S3 board, and install: `PubSubClient`,
`SparkFun MAX3010x`, `Adafruit MPU6050`, `Adafruit Unified Sensor`, `OneWire`,
`DallasTemperature`. Then flash.

### Backend (Raspberry Pi)

```bash
sudo apt install mosquitto mosquitto-clients
pip install flask flask-socketio eventlet paho-mqtt
python backend/server.py
```

The dashboard is then served at `http://<pi-address>:5000`.

---

## Design notes

**The main loop never blocks.** Each sensor has its own interval and is polled against
`millis()`, with timeouts (`MAX_BLOCK_MS`, `MAX_SAMPLE_TIMEOUT_MS`) guarding the slow ones. A
single blocking `delay()` in a system like this would stall every other reading, and a
pulse-oximetry read is slow enough to matter.

**MQTT messages are retained.** A browser opening the dashboard gets the last known value for
every sensor immediately instead of a row of blanks until the next sample.

**Reconnection is handled explicitly.** WiFi and broker connections are retried on an interval
rather than assumed — this ran on a real network, where they drop.

**State lives on the Pi, not the browser.** The backend holds current values, history and alert
state, so refreshing the page or connecting a second browser costs nothing and shows the same
thing.

---

## A note on credentials

WiFi credentials and the broker address are kept in `secrets.h`, which is git-ignored. Only
`secrets.example.h` is committed. The original development version of this firmware had them
hardcoded — worth fixing before anything reaches a public repository.

---

## Stack

ESP32-S3 · Raspberry Pi 4 · C++/Arduino · Python · Flask · Flask-SocketIO · paho-mqtt ·
Mosquitto · Chart.js
