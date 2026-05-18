# FreshScan — UART Protocol

The two ESP32 boards talk over a single UART link (Serial2 on both sides) at **9600 baud, 8N1**. The protocol is line-oriented ASCII — every message ends with `\n`.

There is no handshake, no checksum, and no acknowledgement. The Dev Board enforces a 90-second timeout on the CAM and re-arms itself if no reply arrives.

---

## Wiring

| ESP32 Dev Board | ESP32-CAM | Notes                  |
| --------------- | --------- | ---------------------- |
| GPIO13 (RX)     | GPIO15 (TX) | CAM TX → Dev RX      |
| GPIO14 (TX)     | GPIO14 (RX) | Dev TX → CAM RX      |
| GND             | GND       | **Shared ground required** |

> Both boards configure `Serial2` to these pins via `Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN)`. The CAM's default Serial2 pins (GPIO16/17) are not broken out on the AI Thinker board, so we remap.

---

## Messages

There are exactly two message types.

### 1. Scan request — Dev Board → CAM

```
SCAN:<fruit>\n
```

| Field   | Values                            |
| ------- | --------------------------------- |
| `fruit` | `Apple` · `Banana` · `Orange`     |

Examples:

```
SCAN:Apple\n
SCAN:Banana\n
SCAN:Orange\n
```

### 2. Scan result — CAM → Dev Board

```
<fruit>|<ripeness>|<shelf_life_days>\n
```

| Field              | Values                                              |
| ------------------ | --------------------------------------------------- |
| `fruit`            | `Apple` · `Banana` · `Orange` (echo from API)       |
| `ripeness`         | `Unripe` · `Ripe` · `Rotten` · `Error` · `Unknown`  |
| `shelf_life_days`  | Integer 0–14                                        |

Examples:

```
Banana|Ripe|3\n
Apple|Unripe|7\n
Orange|Rotten|0\n
Banana|Error|0\n
```

`Error` is sent when capture fails, WiFi is down, or the API returns non-200. The Dev Board treats it as `SPOIL` (conservative default) but does not fire the push notification.

---

## Why pipe-delimited?

A single delimiter character keeps parsing trivial on the Dev Board side (`indexOf('|')` twice). JSON over UART would have meant pulling ArduinoJson onto a board already running Blynk + DHT + LCD libraries — fine in principle, but the line-protocol approach has been stable for the entire deployment and is easy to debug with a serial monitor.

---

## Buffer + overflow rules

Both sides accumulate characters into a `String` buffer until they see `\n`. To prevent a stuck or noisy line from filling RAM, each side resets the buffer if it grows past a limit:

- Dev Board: 128 chars
- CAM: 64 chars

Anything received before the first `\n` after boot is discarded, since it may be partial.
