/*
 * ============================================================================
 *  FreshScan — ESP32-CAM firmware (AI Thinker board)
 * ============================================================================
 *  Role of this board:
 *    - Wait on UART Serial2 for "SCAN:<fruit>\n" from the ESP32 Dev Board
 *    - Capture a QVGA JPEG frame
 *    - POST it (multipart/form-data) to the Flask ML API on Render
 *    - Parse the JSON response: { fruit, ripeness, shelf_life_days }
 *    - Send "FRUIT|RIPENESS|DAYS\n" back to the Dev Board over UART
 *
 *  Companion ML API (Flask on Render):
 *    https://github.com/GeetheswarReddy/fresh_scan_api
 *
 *  Board:    AI Thinker ESP32-CAM
 *  Flash:    DIO, 80 MHz
 *  Partition: Huge APP (3MB No OTA / 1MB SPIFFS)
 *  PSRAM:    Disabled (project finding — improved frame-buffer stability)
 *
 *  Author: Geethu (Geetheswar Reddy)
 *  License: MIT
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_camera.h>

// ---------------------------------------------------------------------------
//  WiFi + API
// ---------------------------------------------------------------------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* API_URL   = "https://fresh-scan-api.onrender.com/analyse";

// ---------------------------------------------------------------------------
//  UART to Dev Board (Serial2)
//    On the ESP32-CAM (AI Thinker), GPIO16/17 are not broken out, so we
//    remap Serial2 to GPIO15 (TX) and GPIO14 (RX). These pins are exposed
//    on the side header and are safe to use after boot.
// ---------------------------------------------------------------------------
#define UART_TX_PIN 15   // ESP32-CAM TX → Dev Board RX (GPIO13)
#define UART_RX_PIN 14   // ESP32-CAM RX ← Dev Board TX (GPIO14)

// ---------------------------------------------------------------------------
//  AI Thinker ESP32-CAM pinout — do not change
// ---------------------------------------------------------------------------
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ---------------------------------------------------------------------------
//  Camera init
//    QVGA (320x240) chosen deliberately:
//      - faster uploads on the Render free tier
//      - lower memory pressure → fewer frame-buffer init failures
//    The CNN is trained at 224x224 so QVGA is downscaled server-side.
// ---------------------------------------------------------------------------
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;   // 320x240
  config.jpeg_quality = 12;                // 0..63, lower = better quality
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_DRAM; // PSRAM disabled on this board
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed 0x%x\n", err);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  WiFi
// ---------------------------------------------------------------------------
void connectWiFi() {
  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] OK  IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FAILED — will retry on next scan");
  }
}

// ---------------------------------------------------------------------------
//  Warmup — discard a few frames so the auto-exposure settles before
//  the real capture. Three frames at 200 ms is the sweet spot found
//  during testing.
// ---------------------------------------------------------------------------
void warmupCamera() {
  for (int i = 0; i < 3; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(200);
  }
}

// ---------------------------------------------------------------------------
//  Tiny JSON value extractor — avoids pulling in ArduinoJson for
//  three string fields. Works because the API response is flat.
//    Looks for "key":"value"  (strings)  or  "key":number
// ---------------------------------------------------------------------------
String jsonString(const String& body, const String& key) {
  String needle = "\"" + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return "";
  int colon = body.indexOf(':', k);
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

int jsonInt(const String& body, const String& key) {
  String needle = "\"" + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return -1;
  int colon = body.indexOf(':', k);
  if (colon < 0) return -1;
  int end = body.indexOf(',', colon);
  if (end < 0) end = body.indexOf('}', colon);
  if (end < 0) return -1;
  return body.substring(colon + 1, end).toInt();
}

// ---------------------------------------------------------------------------
//  Send the captured JPEG to the ML API as multipart/form-data
//    Fields:  fruit   — selected fruit name (text)
//             image   — JPEG bytes
//    Returns true on HTTP 200. Out-params filled with parsed JSON.
// ---------------------------------------------------------------------------
bool postToAPI(const String& fruit, camera_fb_t* fb,
               String& outFruit, String& outRipeness, int& outDays) {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation (fine for project scope)

  HTTPClient http;
  if (!http.begin(client, API_URL)) return false;

  String boundary = "----FreshScanBoundary";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.setTimeout(60000);  // accommodate Render cold start

  // Build the multipart body
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"fruit\"\r\n\r\n" +
                fruit + "\r\n"
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"image\"; filename=\"scan.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = head.length() + fb->len + tail.length();
  uint8_t* body = (uint8_t*)malloc(totalLen);
  if (!body) {
    Serial.println("[HTTP] malloc failed");
    http.end();
    return false;
  }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), fb->buf, fb->len);
  memcpy(body + head.length() + fb->len, tail.c_str(), tail.length());

  Serial.printf("[HTTP] POST %s (%u bytes)\n", API_URL, (unsigned)totalLen);
  int code = http.POST(body, totalLen);
  free(body);

  if (code != 200) {
    Serial.printf("[HTTP] failed code=%d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();
  Serial.print("[HTTP] response: ");
  Serial.println(response);

  outFruit    = jsonString(response, "fruit");
  outRipeness = jsonString(response, "ripeness");
  outDays     = jsonInt(response, "shelf_life_days");

  if (outFruit.length() == 0)    outFruit    = fruit;
  if (outRipeness.length() == 0) outRipeness = "Unknown";
  if (outDays < 0)               outDays     = 0;
  return true;
}

// ---------------------------------------------------------------------------
//  Reply to the Dev Board over UART
// ---------------------------------------------------------------------------
void replyToDevBoard(const String& fruit, const String& ripeness, int days) {
  Serial2.print(fruit);
  Serial2.print('|');
  Serial2.print(ripeness);
  Serial2.print('|');
  Serial2.print(days);
  Serial2.print('\n');
  Serial.printf("[UART] -> %s|%s|%d\n", fruit.c_str(), ripeness.c_str(), days);
}

// ---------------------------------------------------------------------------
//  One full scan cycle
// ---------------------------------------------------------------------------
void runScan(const String& fruit) {
  Serial.printf("[SCAN] start fruit=%s\n", fruit.c_str());
  warmupCamera();

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAM] capture failed");
    replyToDevBoard(fruit, "Error", 0);
    return;
  }
  Serial.printf("[CAM] captured %u bytes\n", (unsigned)fb->len);

  String outFruit, outRipeness;
  int outDays = 0;
  bool ok = postToAPI(fruit, fb, outFruit, outRipeness, outDays);
  esp_camera_fb_return(fb);

  if (!ok) {
    replyToDevBoard(fruit, "Error", 0);
    return;
  }
  replyToDevBoard(outFruit, outRipeness, outDays);
}

// ---------------------------------------------------------------------------
//  UART receive — listen for "SCAN:<fruit>\n" from the Dev Board
// ---------------------------------------------------------------------------
String uartBuffer = "";
void pollUart() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n') {
      uartBuffer.trim();
      if (uartBuffer.startsWith("SCAN:")) {
        String fruit = uartBuffer.substring(5);
        fruit.trim();
        if (fruit.length() == 0) fruit = "Apple";
        runScan(fruit);
      } else if (uartBuffer.length() > 0) {
        Serial.print("[UART] ignored: ");
        Serial.println(uartBuffer);
      }
      uartBuffer = "";
    } else {
      uartBuffer += c;
      if (uartBuffer.length() > 64) uartBuffer = "";  // overflow guard
    }
  }
}

// ---------------------------------------------------------------------------
//  Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] ESP32-CAM starting");

  // UART to Dev Board
  Serial2.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  if (!initCamera()) {
    Serial.println("[BOOT] camera init failed — halting");
    while (true) delay(1000);
  }
  Serial.println("[BOOT] camera OK");

  connectWiFi();
  Serial.println("[BOOT] ready, waiting for SCAN commands");
}

// ---------------------------------------------------------------------------
//  Main loop
// ---------------------------------------------------------------------------
void loop() {
  pollUart();
  delay(20);
}
