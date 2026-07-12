#include <M5Unified.h>
#include <WiFi.h>

#include "secrets.h"

namespace {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kRefreshIntervalMs = 1000;
constexpr int32_t kMarginX = 18;
constexpr int32_t kTitleY = 24;
constexpr int32_t kStatusY = 110;
constexpr int32_t kDetailY = 178;

uint32_t lastRefreshMs = 0;

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

void drawConnecting() {
  drawStaticFrame();
  drawTextLine("Connecting...", kStatusY, 3);
  drawTextLine(WIFI_SSID, kDetailY);
  M5.Display.display();
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < kWifiConnectTimeoutMs) {
    M5.delay(250);
  }
}

void drawStatus() {
  drawStaticFrame();

  const bool connected = WiFi.status() == WL_CONNECTED;
  drawTextLine(connected ? "Connected" : "Not connected", kStatusY, 3);

  char ssidText[80];
  snprintf(ssidText, sizeof(ssidText), "SSID: %s", WIFI_SSID);
  drawTextLine(ssidText, kDetailY);

  if (connected) {
    char ipText[80];
    snprintf(ipText, sizeof(ipText), "IP: %s", WiFi.localIP().toString().c_str());
    drawTextLine(ipText, kDetailY + 44);

    char rssiText[80];
    snprintf(rssiText, sizeof(rssiText), "RSSI: %d dBm", WiFi.RSSI());
    drawTextLine(rssiText, kDetailY + 88);
  } else {
    drawTextLine("Check .env and access point.", kDetailY + 44);
  }

  M5.Display.display();
}
}  // namespace

void setup() {
  M5.begin();

  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.setRotation(0);

  drawConnecting();
  connectWifi();
  drawStatus();
  lastRefreshMs = millis();
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  if (now - lastRefreshMs >= kRefreshIntervalMs) {
    lastRefreshMs = now;
    drawStatus();
  }
  M5.delay(10);
}
