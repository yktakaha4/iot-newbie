#include <M5Unified.h>
#include <WiFi.h>

#include "secrets.h"

namespace {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr int32_t kMarginX = 18;
constexpr int32_t kTitleY = 24;
constexpr int32_t kStatusY = 110;
constexpr int32_t kDetailY = 178;
constexpr int32_t kButtonX = 76;
constexpr int32_t kButtonY = 830;
constexpr int32_t kButtonW = 388;
constexpr int32_t kButtonH = 76;

struct WifiCheckResult {
  bool checked = false;
  bool connected = false;
  bool timedOut = false;
  wl_status_t status = WL_IDLE_STATUS;
  uint32_t elapsedMs = 0;
};

WifiCheckResult lastResult;

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

const char* wifiFailureHint(wl_status_t status, bool timedOut) {
  if (timedOut) {
    return "Timeout: check signal, SSID, password, AP.";
  }

  switch (status) {
    case WL_NO_SSID_AVAIL:
      return "SSID not found. Check SSID and AP range.";
    case WL_CONNECT_FAILED:
      return "Connect failed. Check password/auth.";
    case WL_CONNECTION_LOST:
      return "Connection lost. Check signal stability.";
    case WL_DISCONNECTED:
      return "Disconnected. AP may reject credentials.";
    case WL_IDLE_STATUS:
      return "Still idle. Attempt may not have started.";
    default:
      return "Unexpected status. Check AP settings.";
  }
}

void drawTextLine(const char* text, int32_t y, uint8_t textSize = 2) {
  M5.Display.fillRect(kMarginX, y, M5.Display.width() - kMarginX * 2, 34, TFT_WHITE);
  M5.Display.setTextSize(textSize);
  M5.Display.drawString(text, kMarginX, y);
}

void drawStaticFrame() {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(3);
  M5.Display.drawString("WiFi Checker", kMarginX, kTitleY);
}

void drawButton(const char* label) {
  M5.Display.drawRoundRect(kButtonX, kButtonY, kButtonW, kButtonH, 8, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString(label, kButtonX + kButtonW / 2, kButtonY + kButtonH / 2);
  M5.Display.setTextDatum(top_left);
}

bool isButtonClicked() {
  const auto detail = M5.Touch.getDetail();
  return detail.wasClicked() &&
         detail.x >= kButtonX && detail.x < kButtonX + kButtonW &&
         detail.y >= kButtonY && detail.y < kButtonY + kButtonH;
}

void drawStatus() {
  drawStaticFrame();

  drawTextLine(lastResult.connected ? "Connected" : "Not connected", kStatusY, 3);

  char ssidText[80];
  snprintf(ssidText, sizeof(ssidText), "SSID: %s", WIFI_SSID);
  drawTextLine(ssidText, kDetailY);

  char elapsedText[80];
  snprintf(elapsedText, sizeof(elapsedText), "Elapsed: %lu ms", static_cast<unsigned long>(lastResult.elapsedMs));
  drawTextLine(elapsedText, kDetailY + 44);

  char statusText[80];
  snprintf(statusText, sizeof(statusText), "Status: %s (%d)", wifiStatusName(lastResult.status), static_cast<int>(lastResult.status));
  drawTextLine(statusText, kDetailY + 88);

  if (lastResult.connected) {
    char ipText[80];
    snprintf(ipText, sizeof(ipText), "IP: %s", WiFi.localIP().toString().c_str());
    drawTextLine(ipText, kDetailY + 132);

    char rssiText[80];
    snprintf(rssiText, sizeof(rssiText), "RSSI: %d dBm", WiFi.RSSI());
    drawTextLine(rssiText, kDetailY + 176);
  } else {
    drawTextLine(lastResult.timedOut ? "Timed out: yes" : "Timed out: no", kDetailY + 132);
    drawTextLine(wifiFailureHint(lastResult.status, lastResult.timedOut), kDetailY + 176);
  }

  drawButton("Check Again");
  M5.Display.display();
}

void drawWaiting() {
  drawStaticFrame();
  drawTextLine("Ready", kStatusY, 3);
  drawTextLine("Tap button to check WiFi.", kDetailY);
  drawTextLine(WIFI_SSID, kDetailY + 44);
  drawButton("Check WiFi");
  M5.Display.display();
}

void drawConnecting() {
  drawStaticFrame();
  drawTextLine("Connecting...", kStatusY, 3);
  drawTextLine(WIFI_SSID, kDetailY);
  drawButton("Checking...");
  M5.Display.display();
}

wl_status_t connectWifi(uint32_t& elapsedMs, bool& timedOut) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

  Serial.println();
  Serial.println("WiFi check started");
  Serial.printf("SSID: %s\n", WIFI_SSID);
  Serial.printf("Timeout: %lu ms\n", static_cast<unsigned long>(kWifiConnectTimeoutMs));

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < kWifiConnectTimeoutMs) {
    M5.update();
    M5.delay(250);
  }

  elapsedMs = millis() - startedAt;
  const wl_status_t status = WiFi.status();
  timedOut = status != WL_CONNECTED && elapsedMs >= kWifiConnectTimeoutMs;

  Serial.printf("Elapsed: %lu ms\n", static_cast<unsigned long>(elapsedMs));
  Serial.printf("Status: %s (%d)\n", wifiStatusName(status), static_cast<int>(status));

  if (status == WL_CONNECTED) {
    Serial.printf("Result: connected\n");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.printf("Result: failed\n");
    Serial.printf("Timed out: %s\n", timedOut ? "yes" : "no");
    Serial.printf("Hint: %s\n", wifiFailureHint(status, timedOut));
  }

  return status;
}

void checkWifi() {
  drawConnecting();
  lastResult.checked = true;
  lastResult.status = connectWifi(lastResult.elapsedMs, lastResult.timedOut);
  lastResult.connected = lastResult.status == WL_CONNECTED;
  drawStatus();
}
}  // namespace

void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.setRotation(0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  drawWaiting();
}

void loop() {
  M5.update();
  if (isButtonClicked()) {
    checkWifi();
  }
  M5.delay(10);
}
