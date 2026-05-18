/*
 * ============================================================================
 *  FreshScan — ESP32 Dev Board firmware
 * ============================================================================
 *  Role of this board:
 *    - Read DHT11 (temp + humidity) and MQ-2 (gas) sensors
 *    - Drive 16x2 I2C LCD and active buzzer
 *    - Talk to Blynk IoT cloud over WiFi (MQTT)
 *    - Trigger ESP32-CAM scans on a 3-hour schedule (or on Blynk V9)
 *    - Receive "FRUIT|RIPENESS\n" from ESP32-CAM over UART Serial2
 *    - Run shelf-life-aware status logic and buzz on imminent spoilage
 *
 *  Companion ML API (Flask on Render):
 *    https://github.com/GeetheswarReddy/fresh_scan_api
 *
 *  Author: Geethu (Geetheswar Reddy)
 *  License: MIT
 * ============================================================================
 */

#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "FreshScan"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// ---------------------------------------------------------------------------
//  WiFi credentials — replace these for your network
// ---------------------------------------------------------------------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// ---------------------------------------------------------------------------
//  Pin map
// ---------------------------------------------------------------------------
#define DHT_PIN        4         // DHT11 data line
#define DHT_TYPE       DHT11
#define MQ2_PIN        34        // MQ-2 analog output (ADC1_CH6)
#define BUZZER_PIN     5         // Active buzzer
#define UART_RX_PIN    13        // Serial2 RX — wired to ESP32-CAM TX (GPIO15)
#define UART_TX_PIN    14        // Serial2 TX — wired to ESP32-CAM RX (GPIO14)
// I2C LCD uses default SDA=21, SCL=22 on the ESP32 Dev Board

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHT_PIN, DHT_TYPE);
BlynkTimer timer;

// State
String currentFruit       = "Apple";   // Set by Blynk V4
String currentRipeness    = "Unripe";  // Last result from CNN
int    currentShelfLife   = 5;         // Days, from regression
String currentStatus      = "FRESH";   // FRESH / RIPEN / RIPE / SPOIL
float  lastTemp           = 0.0;
float  lastHumidity       = 0.0;
int    lastGasRaw         = 0;
float  lastGasVoltage     = 0.0;
bool   awaitingCamera     = false;
unsigned long lastScanMs  = 0;
const unsigned long SCAN_INTERVAL_MS = 3UL * 60UL * 60UL * 1000UL;  // 3 hours

// ---------------------------------------------------------------------------
//  Blynk virtual pins
//    V0 Temperature       V5 Gas Level (raw)
//    V1 Humidity          V6 Fruit Name (echo)
//    V2 Shelf Life (days) V7 Camera Ripeness
//    V3 Status            V8 Gas Voltage
//    V4 Fruit selector    V9 Scan Now button
// ---------------------------------------------------------------------------

BLYNK_WRITE(V4) {
  int idx = param.asInt();
  switch (idx) {
    case 1: currentFruit = "Apple";  break;
    case 2: currentFruit = "Banana"; break;
    case 3: currentFruit = "Orange"; break;
    default: currentFruit = "Apple"; break;
  }
  Serial.print("[Blynk] Fruit selected: ");
  Serial.println(currentFruit);
  Blynk.virtualWrite(V6, currentFruit);
}

BLYNK_WRITE(V9) {
  if (param.asInt() == 1) {
    Serial.println("[Blynk] Manual scan requested");
    triggerCameraScan();
  }
}

// ---------------------------------------------------------------------------
//  Trigger a camera scan via UART
//    Sends the selected fruit name to the CAM so it can attach it to the
//    POST request to the ML API.
// ---------------------------------------------------------------------------
void triggerCameraScan() {
  if (awaitingCamera) {
    Serial.println("[CAM] Scan already in progress, skipping");
    return;
  }
  awaitingCamera = true;
  lastScanMs = millis();

  Serial2.print("SCAN:");
  Serial2.print(currentFruit);
  Serial2.print("\n");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Scanning...");
  lcd.setCursor(0, 1);
  lcd.print(currentFruit);
  Serial.println("[UART] Scan command sent to CAM");
}

// ---------------------------------------------------------------------------
//  Read sensors and push to Blynk + LCD
//    Runs on a timer independent of the camera, so sensor data never stalls.
// ---------------------------------------------------------------------------
void sendSensorData() {
  float t  = dht.readTemperature();
  float h  = dht.readHumidity();
  int   g  = analogRead(MQ2_PIN);
  float gv = (g / 4095.0) * 3.3;

  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] Read failed");
    return;
  }

  lastTemp       = t;
  lastHumidity   = h;
  lastGasRaw     = g;
  lastGasVoltage = gv;

  // Stagger Blynk writes ≥200 ms apart to respect free-tier flood limits
  Blynk.virtualWrite(V0, t);
  timer.setTimeout(200L, []() { Blynk.virtualWrite(V1, lastHumidity); });
  timer.setTimeout(400L, []() { Blynk.virtualWrite(V5, lastGasRaw); });
  timer.setTimeout(600L, []() { Blynk.virtualWrite(V8, lastGasVoltage); });

  if (!awaitingCamera) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(currentFruit);
    lcd.print(" ");
    lcd.print(currentStatus);
    lcd.setCursor(0, 1);
    lcd.print("T:");
    lcd.print((int)t);
    lcd.print("C H:");
    lcd.print((int)h);
    lcd.print("%");
  }
}

// ---------------------------------------------------------------------------
//  Status derivation from ripeness + shelf life
//    Maps the 3-class CNN output and shelf-life number into one of four
//    user-friendly states shown on V3 and the LCD.
// ---------------------------------------------------------------------------
String deriveStatus(const String& ripeness, int shelfLife) {
  if (ripeness == "Rotten")        return "SPOIL";
  if (shelfLife <= 1)              return "SPOIL";
  if (ripeness == "Ripe")          return "RIPE";
  if (ripeness == "Unripe" && shelfLife <= 3) return "RIPEN";
  return "FRESH";
}

// ---------------------------------------------------------------------------
//  Handle one parsed result from the CAM
// ---------------------------------------------------------------------------
void handleCamResult(const String& fruit, const String& ripeness, int shelfDays) {
  currentFruit      = fruit;
  currentRipeness   = ripeness;
  currentShelfLife  = shelfDays;
  currentStatus     = deriveStatus(ripeness, shelfDays);
  awaitingCamera    = false;

  Serial.printf("[RESULT] %s | %s | %d days | %s\n",
                fruit.c_str(), ripeness.c_str(), shelfDays, currentStatus.c_str());

  Blynk.virtualWrite(V2, shelfDays);
  timer.setTimeout(200L, []() { Blynk.virtualWrite(V3, currentStatus); });
  timer.setTimeout(400L, []() { Blynk.virtualWrite(V6, currentFruit); });
  timer.setTimeout(600L, []() { Blynk.virtualWrite(V7, currentRipeness); });

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(currentFruit);
  lcd.print(" ");
  lcd.print(currentStatus);
  lcd.setCursor(0, 1);
  lcd.print(currentRipeness);
  lcd.print(" ");
  lcd.print(shelfDays);
  lcd.print("d");

  // Alert path: buzzer + push notification
  if (currentStatus == "SPOIL" || shelfDays <= 1) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(800);
    digitalWrite(BUZZER_PIN, LOW);
    Blynk.logEvent("spoiler_alert",
                   String(currentFruit) + " spoiling: " + shelfDays + " day(s) left");
  }
}

// ---------------------------------------------------------------------------
//  Read UART for incoming "FRUIT|RIPENESS|DAYS\n" lines from the CAM
//    Non-blocking — accumulates characters and parses on newline.
// ---------------------------------------------------------------------------
String uartBuffer = "";
void pollUart() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      uartBuffer.trim();
      if (uartBuffer.length() > 0) {
        int firstPipe  = uartBuffer.indexOf('|');
        int secondPipe = uartBuffer.indexOf('|', firstPipe + 1);
        if (firstPipe > 0 && secondPipe > firstPipe) {
          String fruit    = uartBuffer.substring(0, firstPipe);
          String ripeness = uartBuffer.substring(firstPipe + 1, secondPipe);
          int    days     = uartBuffer.substring(secondPipe + 1).toInt();
          handleCamResult(fruit, ripeness, days);
        } else {
          Serial.print("[UART] Malformed line: ");
          Serial.println(uartBuffer);
        }
      }
      uartBuffer = "";
    } else {
      uartBuffer += c;
      if (uartBuffer.length() > 128) uartBuffer = "";  // overflow guard
    }
  }

  // Camera timeout — don't block forever waiting for a reply
  if (awaitingCamera && (millis() - lastScanMs > 90000UL)) {
    Serial.println("[CAM] Scan timed out");
    awaitingCamera = false;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Scan timeout");
  }
}

// ---------------------------------------------------------------------------
//  Auto-scan every 3 hours
// ---------------------------------------------------------------------------
void autoScanCheck() {
  static unsigned long lastAuto = 0;
  if (millis() - lastAuto >= SCAN_INTERVAL_MS) {
    lastAuto = millis();
    Serial.println("[AUTO] 3-hour scan triggered");
    triggerCameraScan();
  }
}

// ---------------------------------------------------------------------------
//  Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("FreshScan");
  lcd.setCursor(0, 1);
  lcd.print("Booting...");

  dht.begin();
  analogReadResolution(12);  // 0..4095

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("FreshScan ready");
  lcd.setCursor(0, 1);
  lcd.print(currentFruit);

  // Schedulers
  timer.setInterval(5000L,  sendSensorData);   // every 5 s
  timer.setInterval(10000L, autoScanCheck);    // check 3-hour window
  timer.setInterval(50L,    pollUart);         // fast UART polling

  Serial.println("[BOOT] Dev Board ready");
}

// ---------------------------------------------------------------------------
//  Main loop
// ---------------------------------------------------------------------------
void loop() {
  Blynk.run();
  timer.run();
}
