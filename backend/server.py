#!/usr/bin/env python3
"""
Health Monitor Backend — Raspberry Pi 4
Bridges ESP32 MQTT sensor data to browser via WebSocket.

Run:
  source venv/bin/activate
  python backend/server.py

Author: Senator Chigoziri Ugwu
"""

import os
import json
import time
import threading
from datetime import datetime
from flask import Flask, send_from_directory
from flask_socketio import SocketIO
import paho.mqtt.client as mqtt

# ── Config ──────────────────────────────────────────────────────
MQTT_BROKER   = "localhost"
MQTT_PORT     = 1883
MQTT_TOPIC    = "health/#"

FLASK_HOST    = "0.0.0.0"
FLASK_PORT    = 5000

# Alert thresholds
TEMP_MIN      = 35.0
TEMP_MAX      = 38.5
HR_MIN        = 50
HR_MAX        = 120
SPO2_MIN      = 95
INACTIVITY_ALERT_SECONDS = 3600

# ── App Setup ────────────────────────────────────────────────────
app = Flask(__name__, static_folder="static")
app.config["SECRET_KEY"] = os.environ.get("SECRET_KEY", "dev-only-change-me")
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="eventlet",
                    ping_timeout=10, ping_interval=5)

# In-memory state
sensor_state = {
    "temperature": None,
    "heartrate":   None,
    "spo2":        None,
    "weight":      None,
    "movement":    None,
    "last_movement_time": None,
    "last_updated": None,
    "alerts": []
}

history = {
    "temperature": [],
    "heartrate":   [],
    "spo2":        [],
    "weight":      [],
}
MAX_HISTORY = 120

# ── Alert Logic ──────────────────────────────────────────────────
def build_alerts(state):
    alerts = []
    now = datetime.now().isoformat()

    temp = state.get("temperature")
    hr   = state.get("heartrate")
    spo2 = state.get("spo2")

    if temp is not None:
        if temp > TEMP_MAX:
            alerts.append({"id": "temp_high", "level": "danger",
                "message": f"High temperature: {temp:.1f}°C — possible fever", "time": now})
        elif temp < TEMP_MIN:
            alerts.append({"id": "temp_low", "level": "warning",
                "message": f"Low temperature: {temp:.1f}°C — possible hypothermia", "time": now})

    if hr is not None:
        if hr > HR_MAX:
            alerts.append({"id": "hr_high", "level": "danger",
                "message": f"High heart rate: {hr} BPM — tachycardia risk", "time": now})
        elif hr < HR_MIN:
            alerts.append({"id": "hr_low", "level": "warning",
                "message": f"Low heart rate: {hr} BPM — bradycardia risk", "time": now})

    if spo2 is not None:
        if spo2 < 90:
            alerts.append({"id": "spo2_critical", "level": "danger",
                "message": f"Critical SpO2: {spo2}% — immediate attention needed!", "time": now})
        elif spo2 < SPO2_MIN:
            alerts.append({"id": "spo2_low", "level": "warning",
                "message": f"Low SpO2: {spo2}% — monitor closely", "time": now})

    last_mov = state.get("last_movement_time")
    if last_mov is not None:
        seconds_since = time.time() - last_mov
        if seconds_since > INACTIVITY_ALERT_SECONDS:
            mins = int(seconds_since // 60)
            alerts.append({"id": "inactivity", "level": "warning",
                "message": f"No bed movement for {mins} minutes — check on patient", "time": now})

    return alerts

def push_state():
    sensor_state["alerts"] = build_alerts(sensor_state)
    socketio.emit("sensor_update", {
        "state":   sensor_state,
        "history": history
    })

# ── MQTT Callbacks ───────────────────────────────────────────────
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] Connected to broker at {MQTT_BROKER}:{MQTT_PORT}")
        client.subscribe(MQTT_TOPIC)
        print(f"[MQTT] Subscribed to '{MQTT_TOPIC}'")
    else:
        print(f"[MQTT] Connection failed, rc={rc}")

def on_message(client, userdata, msg):
    topic   = msg.topic
    payload = msg.payload.decode("utf-8").strip()
    now_ts  = datetime.now().isoformat()

    print(f"[MQTT] {topic}: {payload}")

    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        try:
            data = float(payload)
        except ValueError:
            print(f"[MQTT] Could not parse payload: {payload}")
            return

    sensor = topic.split("/")[-1]
    sensor_state["last_updated"] = now_ts

    if sensor == "temperature":
        val = float(data) if not isinstance(data, dict) else float(data["value"])
        sensor_state["temperature"] = round(val, 1)
        _append_history("temperature", val)

    elif sensor == "heartrate":
        val = int(data) if not isinstance(data, dict) else int(data["value"])
        sensor_state["heartrate"] = val
        _append_history("heartrate", val)

    elif sensor == "spo2":
        val = int(float(data)) if not isinstance(data, dict) else int(data["value"])
        sensor_state["spo2"] = val
        _append_history("spo2", val)

    elif sensor == "weight":
        val = float(data) if not isinstance(data, dict) else float(data["value"])
        sensor_state["weight"] = round(val, 1)
        _append_history("weight", val)

    elif sensor == "movement":
        val = bool(int(float(data))) if isinstance(data, (int, float, str)) else bool(data)
        sensor_state["movement"] = val
        if val:
            sensor_state["last_movement_time"] = time.time()

    push_state()

def _append_history(key, value):
    history[key].append({"t": datetime.now().isoformat(), "v": value})
    if len(history[key]) > MAX_HISTORY:
        history[key].pop(0)

# ── MQTT Thread ──────────────────────────────────────────────────
def start_mqtt():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()

# ── Flask Routes ─────────────────────────────────────────────────
@app.route("/")
def index():
    return send_from_directory("../dashboard", "index.html")

@app.route("/api/state")
def api_state():
    return {"state": sensor_state, "history": history}

@socketio.on("connect")
def on_ws_connect():
    print("[WS] Browser connected")
    push_state()

@socketio.on("disconnect")
def on_ws_disconnect():
    print("[WS] Browser disconnected")

# ── Entry Point ───────────────────────────────────────────────────
if __name__ == "__main__":
    mqtt_thread = threading.Thread(target=start_mqtt, daemon=True)
    mqtt_thread.start()

    print(f"\n{'='*50}")
    print(f"  Health Monitor Backend")
    print(f"  Dashboard: http://{FLASK_HOST}:{FLASK_PORT}")
    print(f"  MQTT:      {MQTT_BROKER}:{MQTT_PORT}  topic={MQTT_TOPIC}")
    print(f"{'='*50}\n")

    socketio.run(app, host=FLASK_HOST, port=FLASK_PORT, debug=False)
