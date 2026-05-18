# FreshScan — Setup Guide

## 1. Arduino IDE setup

Install the **ESP32 board package** (Espressif Systems) via Boards Manager.

Install these libraries via Library Manager:

- `Blynk` (by Volodymyr Shymanskyy)
- `LiquidCrystal_I2C` (by Frank de Brabander)
- `DHT sensor library` (by Adafruit) + `Adafruit Unified Sensor`

## 2. Configure credentials

Open both `.ino` files and replace these placeholders:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
```

In `ESP32_DevBoard.ino` also set:

```cpp
#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "FreshScan"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"
```

Get the template ID and auth token from the Blynk web console after creating a template with the V0–V9 pin layout described in [`WORKFLOW.md`](./WORKFLOW.md).

## 3. Wiring

Follow [`../hardware/connection_diagram.svg`](../hardware/connection_diagram.svg). The critical bits:

- **Common ground** between both ESP32 boards — without it, the UART line is meaningless.
- **CAM TX (GPIO15) → Dev RX (GPIO13)** and **Dev TX (GPIO14) → CAM RX (GPIO14)**. RX↔TX is crossed.
- DHT11 data on GPIO4 with a 10 kΩ pull-up to 3.3 V.
- MQ-2 analog out on GPIO34 (input-only ADC pin).
- I2C LCD on SDA=GPIO21, SCL=GPIO22 (default).
- Active buzzer on GPIO5.
- Power the ESP32-CAM from a **separate 5 V supply** (or the CH340 adapter's 5 V) — the 3.3 V regulator on the Dev Board cannot reliably source the camera's inrush current.

## 4. Flashing

### ESP32 Dev Board

Board: **ESP32 Dev Module**
Port: macOS `/dev/cu.usbserial-0001` · Windows `COM4` (CH340)
Upload speed: 921600

### ESP32-CAM (AI Thinker)

Connect via a CH340 USB-TTL adapter. Wiring during programming:

| CH340 | ESP32-CAM |
| ----- | --------- |
| 5V    | 5V        |
| GND   | GND       |
| TX    | U0R       |
| RX    | U0T       |

Then **short IO0 to GND** before powering up. After upload, remove the short and reset.

Arduino IDE settings:

- Board: **AI Thinker ESP32-CAM**
- Flash Mode: **DIO**
- Partition Scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)**
- PSRAM: **Disabled** (project-specific finding — improves frame-buffer stability on this board batch)

## 5. Blynk app setup

Create a Blynk template with these datastreams:

| Pin | Type     | Min  | Max  |
| --- | -------- | ---- | ---- |
| V0  | Double   | -10  | 60   |
| V1  | Double   | 0    | 100  |
| V2  | Integer  | 0    | 14   |
| V3  | String   | —    | —    |
| V4  | Integer  | 1    | 3    |
| V5  | Integer  | 0    | 4095 |
| V6  | String   | —    | —    |
| V7  | String   | —    | —    |
| V8  | Double   | 0    | 3.3  |
| V9  | Integer  | 0    | 1    |

Add an **event** with code `spoiler_alert` and enable push notifications on it.

## 6. First boot

1. Power the Dev Board → LCD shows `FreshScan / Booting...` then `FreshScan ready / Apple`.
2. Power the ESP32-CAM → serial monitor at 115200 shows `[BOOT] camera OK` and `[WiFi] OK`.
3. Open the Blynk app → V0/V1/V5/V8 start updating within 5 seconds.
4. Tap **Scan Now** (V9) → LCD shows `Scanning... / Apple`. Within a few seconds (or up to ~50 s on cold start), V2/V3/V6/V7 populate.

## Troubleshooting

| Symptom                                     | Likely cause                                                |
| ------------------------------------------- | ----------------------------------------------------------- |
| Camera init error `0x105`                   | Ribbon cable not fully seated, or 5 V supply too weak       |
| LCD blank or garbage                        | Wrong I2C address — try `0x3F` instead of `0x27`             |
| UART silence between boards                 | Missing common ground, or TX/RX not crossed                 |
| First scan after idle takes 30–50 s         | Normal — Render free-tier cold start                        |
| Blynk says "device offline"                 | Wrong auth token, or WiFi credentials wrong on Dev Board    |
| Buzzer never fires                          | Status only triggers on `SPOIL` or `shelf_life_days ≤ 1`    |
