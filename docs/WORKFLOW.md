# FreshScan — Workflow

This document walks through one full scan cycle, end to end. It explains what each board does, when it does it, and what data flows between them.

---

## Roles at a glance

| Board               | Responsible for                                                       |
| ------------------- | --------------------------------------------------------------------- |
| **ESP32 Dev Board** | Sensors (DHT11, MQ-2), LCD, buzzer, Blynk MQTT, scan scheduling       |
| **ESP32-CAM**       | Camera capture, WiFi + HTTPS, calling the ML API, parsing JSON        |

The split exists for two reasons:

1. The AI Thinker ESP32-CAM has very few free GPIOs once the camera + flash + SD slot pins are accounted for. Hanging a DHT11, MQ-2, LCD and buzzer off it is fragile.
2. Doing camera + WiFi + sensor sampling on one board causes timing jitter — the sensor readings drift while the camera is uploading. Splitting them keeps the sensor loop clean.

---

## The two phases

There are two independent loops running:

### Phase A — sensor loop (continuous, every 5 seconds)

Owned by the Dev Board. Never touches the camera or the cloud.

```
DHT11 + MQ-2 → Dev Board
                  │
                  ├─► LCD update (line 2: T:27C H:62%)
                  └─► Blynk.virtualWrite(V0, V1, V5, V8)
                          (writes are spaced ≥200 ms apart)
```

This loop runs forever, regardless of whether a scan is in flight.

### Phase B — scan cycle (every 3 hours, or on demand)

Triggered either by the 3-hour timer on the Dev Board, or by the user tapping the **Scan Now** button (V9) in the Blynk app.

```
1. Dev Board     → Serial2 → "SCAN:Banana\n"          → ESP32-CAM
2. ESP32-CAM     warms up camera (3 frames, 200 ms apart)
3. ESP32-CAM     captures QVGA JPEG (320×240)
4. ESP32-CAM     POST multipart to Flask API on Render
                 ├─ field "fruit"  = "Banana"
                 └─ field "image"  = <JPEG bytes>
5. Flask API     runs MobileNetV2 → ripeness label
                 runs regression  → shelf_life_days
                 returns JSON: { fruit, ripeness, shelf_life_days, status }
6. ESP32-CAM     parses JSON
7. ESP32-CAM     → Serial2 → "Banana|Ripe|3\n"        → Dev Board
8. Dev Board     parses line, updates state
9. Dev Board     updates LCD + Blynk (V2, V3, V6, V7)
10. If shelf_life_days ≤ 1 → buzzer + push notification
```

End to end this takes roughly **3–5 seconds** on a warm API, or **30–50 seconds** if the Render free-tier dyno is cold.

---

## Blynk virtual pin map

| Pin | Direction | Meaning                                       |
| --- | --------- | --------------------------------------------- |
| V0  | → app     | Temperature (°C)                              |
| V1  | → app     | Humidity (%)                                  |
| V2  | → app     | Shelf life (days)                             |
| V3  | → app     | Status — `FRESH` / `RIPEN` / `RIPE` / `SPOIL` |
| V4  | ← app     | Fruit selector (1=Apple, 2=Banana, 3=Orange)  |
| V5  | → app     | Gas level (raw ADC, 0–4095)                   |
| V6  | → app     | Fruit name echo                               |
| V7  | → app     | Camera ripeness label                         |
| V8  | → app     | Gas voltage (V)                               |
| V9  | ← app     | Scan Now button                               |

The `spoiler_alert` Blynk **event** fires the push notification on `shelf_life_days ≤ 1` or `status == SPOIL`.

---

## Status derivation

Status is computed on the Dev Board, not the API. The rule is intentionally simple so it can be explained on the LCD:

```
if  ripeness == "Rotten"                 → SPOIL
elif shelf_life_days <= 1                → SPOIL
elif ripeness == "Ripe"                  → RIPE
elif ripeness == "Unripe" and days <= 3  → RIPEN     (will be ripe soon)
else                                      → FRESH
```

---

## Rotten bypass (an important detail)

If the CNN classifies the fruit as **Rotten**, the API skips the regression model entirely and returns `shelf_life_days = 0`. This is deliberate. Without the bypass you can get nonsense like "Rotten — 4 days left" when the sensors happen to be in a fresh-looking range. The visible state of the fruit always wins over the regression's optimism.

---

## Auto-scan vs. manual scan

- **Auto-scan** — fires every 3 hours from `autoScanCheck()`. Tuned for a piece of fruit sitting on a counter; the system isn't designed to monitor at higher frequencies and the Render free tier wouldn't sustain it anyway.
- **Manual scan** — fires on Blynk V9. Used for debugging and for the demo. The Dev Board guards against overlapping scans with the `awaitingCamera` flag and a 90-second timeout.

---

## Failure paths

| Failure                          | Behaviour                                                    |
| -------------------------------- | ------------------------------------------------------------ |
| DHT11 read fails (NaN)           | Skip the cycle, leave previous values on LCD                 |
| WiFi down on the CAM             | Reconnect attempt, then `Error` reply over UART              |
| Render cold start (slow)         | HTTP timeout set to 60 s, then `Error` reply if exceeded     |
| CAM doesn't reply within 90 s    | Dev Board clears `awaitingCamera`, shows "Scan timeout"      |
| Malformed UART line              | Logged to Serial, line dropped                               |
| Buffer overflow on UART          | Buffer reset (>128 chars on Dev, >64 on CAM)                 |

---

## See also

- [`PROTOCOL.md`](./PROTOCOL.md) — exact UART message format
- [`SETUP.md`](./SETUP.md) — flashing, wiring, and bring-up
- [`../hardware/connection_diagram.svg`](../hardware/connection_diagram.svg) — wiring map
- [ML API repo](https://github.com/GeetheswarReddy/fresh_scan_api) — Flask app, training code, model files
