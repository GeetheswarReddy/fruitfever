# 🍎 FreshScan — IoT Fruit Freshness Detection System

An end-to-end IoT system that detects fruit ripeness and predicts shelf life in days. Built on **dual ESP32 boards** that talk to each other over UART, with environmental sensing on one side and computer vision on the other. Predictions come from a cloud-hosted ML API and are pushed live to a phone dashboard.

> **Status:** Completed & handed over · Embedded systems + IoT + Cloud ML integration

---

## What it does

FreshScan watches a fruit (Apple, Banana, or Orange) and answers two questions in real time:

1. **How ripe is it right now?** — `Unripe` / `Ripe` / `Rotten` (from a CNN looking at a live camera frame)
2. **How many days until it spoils?** — a number in days (from a regression model fed sensor + ripeness data)

Sensor readings stream every few seconds. The camera scans every 3 hours, or instantly on demand from the Blynk app. When shelf life drops to ≤ 1 day, the buzzer fires and a push notification hits the user's phone.

---

## System architecture

```
┌─────────────────────┐                 ┌─────────────────────┐
│   ESP32-CAM         │   UART @ 9600   │   ESP32 Dev Board   │
│   (AI Thinker)      │  ─────────────▶ │   (Main controller) │
│                     │  "FRUIT|RIPE\n" │                     │
│  • Captures JPEG    │                 │  • DHT11 (T + RH)   │
│  • POSTs to ML API  │                 │  • MQ-2 gas sensor  │
│  • Parses response  │                 │  • 16x2 I2C LCD     │
│  • Forwards result  │                 │  • Active buzzer    │
└──────────┬──────────┘                 └──────────┬──────────┘
           │                                       │
           │ HTTPS POST                            │ MQTT
           │ multipart/form-data                   │ (Blynk lib)
           ▼                                       ▼
┌─────────────────────┐                 ┌─────────────────────┐
│  Flask ML API       │                 │  Blynk IoT Cloud    │
│  on Render.com      │                 │                     │
│                     │                 │  Phone dashboard    │
│  • MobileNetV2 CNN  │                 │  + push alerts      │
│  • Linear regr.     │                 │                     │
│    (shelf life)     │                 │                     │
└─────────────────────┘                 └─────────────────────┘
```

The two ESP32 boards have **separate roles** and **separate power**. They are linked by a 3-wire UART connection (TX, RX, GND). The Dev Board never talks directly to the ML API — that's the CAM's job. The Dev Board never talks directly to the camera — that's also the CAM's job. Each board owns its slice of the work.

---

## Team

FreshScan was built by **Geetheswar Reddy** and **Himanshu Sastry** as a final-year project. Himanshu led the IoT side and the ESP32-CAM firmware; the rest of the system was built jointly, with most design decisions and debugging done together.

---

## Repository layout

```
FreshScan/
├── firmware/
│   ├── ESP32_DevBoard/         # Sensors, LCD, buzzer, Blynk, UART RX
│   │   └── ESP32_DevBoard.ino
│   └── ESP32_CAM/              # Camera, WiFi, ML API call, UART TX
│       └── ESP32_CAM.ino
├── hardware/
│   ├── connection_diagram.svg  # Full wiring diagram (see below)
│   └── workflow_diagram.svg    # End-to-end scan-cycle flow
├── docs/
│   ├── WORKFLOW.md             # Step-by-step flow of one full scan cycle
│   ├── PROTOCOL.md             # UART protocol between the two boards
│   └── SETUP.md                # How to flash, wire, and run it
├── .gitignore
└── README.md
```

> 🔗 **Companion ML repo (Flask API + training notebooks):** [GeetheswarReddy/fresh_scan_api](https://github.com/GeetheswarReddy/fresh_scan_api)

---

## Hardware

| Component        | Role                                                  |
| ---------------- | ----------------------------------------------------- |
| ESP32 Dev Board  | Main controller — sensors, LCD, buzzer, Blynk MQTT    |
| ESP32-CAM (AI Thinker) | Camera capture + HTTPS call to ML API           |
| DHT11            | Temperature + humidity                                |
| MQ-2 gas sensor  | Ethylene/VOC proxy (analog out)                       |
| 16×2 I2C LCD     | On-device readout                                     |
| Active buzzer    | Spoilage alert                                        |
| CH340 USB-TTL    | Programming adapter for the ESP32-CAM                 |

See [`hardware/connection_diagram.svg`](./hardware/connection_diagram.svg) for the full wiring map.

---

## Workflow (one full cycle)

1. User picks the fruit (Apple / Banana / Orange) on the Blynk app (V4).
2. Dev Board reads DHT11 + MQ-2 every few seconds and pushes values to Blynk (V0, V1, V5, V8) and to the LCD.
3. Every 3 hours (or when V9 "Scan Now" is tapped), the Dev Board signals the CAM over UART to scan.
4. ESP32-CAM captures a QVGA JPEG and POSTs it to `https://fresh-scan-api.onrender.com/analyse` along with the fruit name.
5. The Flask API runs the MobileNetV2 CNN, gets a ripeness label, runs the regression model on `(fruit, temp, RH, gas, ripeness)`, and returns `{fruit, ripeness, shelf_life_days, status}`.
6. ESP32-CAM forwards `"FRUIT|RIPENESS\n"` back over UART. The Dev Board parses it, updates the LCD and Blynk (V2, V3, V6, V7).
7. If shelf life ≤ 1 day → buzzer fires + Blynk push notification (`spoiler_alert` event).

A more detailed step-by-step (with the rotten-bypass logic and Blynk virtual pin map) lives in [`docs/WORKFLOW.md`](./docs/WORKFLOW.md). The visual end-to-end flow is in [`hardware/workflow_diagram.svg`](./hardware/workflow_diagram.svg).

---

## What we built

The system spans embedded firmware, computer vision, regression modelling, cloud deployment, and a phone dashboard. Some pieces were owned individually, but most of the integration was joint work:

- **Embedded firmware** for two ESP32 boards coordinating over UART, with non-blocking timers for sensor sampling, Blynk transmission (throttled ≥200 ms apart to respect free-tier flood limits), and camera scheduling.
- **Inter-board protocol** — a tiny pipe-delimited line protocol (`FRUIT|RIPENESS\n`) so the Dev Board can stay sensor-focused while the CAM handles networking and vision. The split-of-responsibilities was a team design decision after we hit GPIO contention trying to do everything on one board.
- **Cloud ML integration** — multipart image upload from a microcontroller to a Flask API on Render, with cold-start latency handling and graceful failure paths.
- **Phone dashboard** — Blynk virtual pin mapping for live data + manual scan trigger + push notifications on the `spoiler_alert` event.
- **Rotten-bypass logic** — when the CNN sees rotten fruit, the regression model is skipped entirely and shelf life is forced to 0 days. This caught the "sensors look fine but the fruit is visibly gone" case we kept hitting during testing, and the fix came out of a team review session.

---

## Known limitations (disclosed to client)

- **Cold start latency** — the Flask API sleeps on Render's free tier. First scan after inactivity takes ~30–50 seconds.
- **CNN has no "no fruit" class** — the model always returns one of its 9 trained classes. Pointing the camera at empty space will still produce a (meaningless) prediction.
- **QVGA capture (320×240)** — chosen for upload speed and frame-buffer stability on the AI Thinker board. Higher resolutions caused intermittent capture failures.
- **3 fruits only** — Apple, Banana, Orange. Adding fruits requires retraining the CNN and re-fitting the regression model.

---

## Tech stack

**Embedded:** Arduino-ESP32, Blynk, LiquidCrystal_I2C, DHT, HTTPClient, esp_camera
**Cloud:** Flask, TensorFlow / Keras (MobileNetV2), scikit-learn, Render.com
**IoT:** Blynk IoT (MQTT) — phone dashboard + push notifications
**Protocol:** UART Serial2 @ 9600 baud, pipe-delimited ASCII

---

## License

MIT — free to fork, learn from, and remix.
