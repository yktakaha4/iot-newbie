#include <Ambient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <stdlib.h>

#include "secrets.h"

namespace {
constexpr uint8_t kSht30Address = 0x44;
constexpr uint32_t kI2cFrequency = 400000;
constexpr uint32_t kSensorReadIntervalMs = 10000;
constexpr uint32_t kDisplayRefreshIntervalMs = 1000;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kAmbientTimeoutMs = 5000;
constexpr size_t kVisibleRows = 11;
constexpr int32_t kMarginX = 14;
constexpr int32_t kTitleY = 18;
constexpr int32_t kSsidY = 54;
constexpr int32_t kAmbientChannelY = 78;
constexpr int32_t kAmbientFieldsY = 102;
constexpr int32_t kBatteryY = 126;
constexpr int32_t kWifiStatusY = 152;
constexpr int32_t kTableTop = 196;
constexpr int32_t kRowsTop = kTableTop + 44;
constexpr int32_t kRowHeight = 52;
constexpr int32_t kColTime = 14;
constexpr int32_t kColTemp = 132;
constexpr int32_t kColHum = 232;
constexpr int32_t kColWifi = 332;
constexpr int32_t kColAmbient = 420;
constexpr int32_t kTableRight = 526;
constexpr int32_t kStatusY = 830;
constexpr int32_t kButtonX = 76;
constexpr int32_t kButtonY = 874;
constexpr int32_t kButtonW = 388;
constexpr int32_t kButtonH = 64;

uint32_t lastSensorReadMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t sampleCount = 0;
size_t nextDisplayRow = 0;
bool hasPendingReading = false;
bool wifiConnecting = false;
bool wifiLastTimedOut = false;
uint32_t wifiLastElapsedMs = 0;
wl_status_t wifiLastStatus = WL_IDLE_STATUS;
WiFiClient ambientClient;
Ambient ambient;
bool ambientReady = false;

struct AmbientResult {
  bool attempted = false;
  bool ok = false;
  int httpCode = 0;
};

struct Reading {
  bool sensorOk = false;
  bool wifiOk = false;
  m5::rtc_datetime_t datetime;
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
  AmbientResult ambient;
};

Reading latestReading;

uint8_t crc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool readSht30(float& temperatureC, float& humidityPercent) {
  const uint8_t measureCommand[] = {0x24, 0x00};
  uint8_t data[6] = {};

  if (!M5.In_I2C.start(kSht30Address, false, kI2cFrequency)) {
    return false;
  }
  bool ok = M5.In_I2C.write(measureCommand, sizeof(measureCommand));
  ok = M5.In_I2C.stop() && ok;
  if (!ok) {
    return false;
  }

  M5.delay(20);

  if (!M5.In_I2C.start(kSht30Address, true, kI2cFrequency)) {
    return false;
  }
  ok = M5.In_I2C.read(data, sizeof(data), true);
  ok = M5.In_I2C.stop() && ok;
  if (!ok) {
    return false;
  }

  if (crc8(data, 2) != data[2] || crc8(&data[3], 2) != data[5]) {
    return false;
  }

  const uint16_t rawTemperature = static_cast<uint16_t>(data[0] << 8 | data[1]);
  const uint16_t rawHumidity = static_cast<uint16_t>(data[3] << 8 | data[4]);
  temperatureC = -45.0f + 175.0f * static_cast<float>(rawTemperature) / 65535.0f;
  humidityPercent = 100.0f * static_cast<float>(rawHumidity) / 65535.0f;
  return true;
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN";
    case WL_CONNECTED:
      return "OK";
    case WL_CONNECT_FAILED:
      return "FAILED";
    case WL_CONNECTION_LOST:
      return "LOST";
    case WL_DISCONNECTED:
      return "DISCONN";
    default:
      return "UNKNOWN";
  }
}

void formatTime(const m5::rtc_time_t& time, char* buffer, size_t length) {
  snprintf(buffer, length, "%02d:%02d:%02d", time.hours, time.minutes, time.seconds);
}

void drawButton(const char* label) {
  M5.Display.fillRect(kButtonX - 2, kButtonY - 2, kButtonW + 4, kButtonH + 4, TFT_WHITE);
  M5.Display.drawRoundRect(kButtonX, kButtonY, kButtonW, kButtonH, 8, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString(label, kButtonX + kButtonW / 2, kButtonY + kButtonH / 2);
  M5.Display.setTextDatum(top_left);
}

bool isReconnectButtonClicked() {
  const auto detail = M5.Touch.getDetail();
  return detail.wasClicked() &&
         detail.x >= kButtonX && detail.x < kButtonX + kButtonW &&
         detail.y >= kButtonY && detail.y < kButtonY + kButtonH;
}

void drawWifiStatus() {
  M5.Display.fillRect(kMarginX, kWifiStatusY, kTableRight - kMarginX, 34, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char statusText[96];
  if (wifiConnecting) {
    snprintf(statusText, sizeof(statusText), "WiFi: connecting to %s", WIFI_SSID);
  } else if (wifiLastStatus == WL_CONNECTED) {
    snprintf(statusText, sizeof(statusText), "WiFi: OK  IP %s  RSSI %d dBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    snprintf(statusText, sizeof(statusText), "WiFi: %s  elapsed %lums%s",
             wifiStatusName(wifiLastStatus),
             static_cast<unsigned long>(wifiLastElapsedMs),
             wifiLastTimedOut ? " timeout" : "");
  }
  M5.Display.drawString(statusText, kMarginX, kWifiStatusY);
}

void drawBatteryStatus() {
  M5.Display.fillRect(kMarginX, kBatteryY, kTableRight - kMarginX, 24, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char batteryText[64];
  const int batteryLevel = M5.Power.getBatteryLevel();
  snprintf(batteryText, sizeof(batteryText), "Battery: %d%% %s",
           batteryLevel,
           M5.Power.isCharging() ? "charging" : "not charging");
  M5.Display.drawString(batteryText, kMarginX, kBatteryY);
}

void drawDeviceInfo() {
  char ssidText[96];
  char ambientChannelText[64];
  snprintf(ssidText, sizeof(ssidText), "SSID: %s", WIFI_SSID);
  snprintf(ambientChannelText, sizeof(ambientChannelText), "Ambient Ch: %s", AMBIENT_CHANNEL_ID);

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.drawString(ssidText, kMarginX, kSsidY);
  M5.Display.drawString(ambientChannelText, kMarginX, kAmbientChannelY);
  M5.Display.drawString("Ambient fields: d1=temp d2=hum", kMarginX, kAmbientFieldsY);
  drawBatteryStatus();
}

void drawStaticFrame() {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);

  M5.Display.setTextSize(3);
  M5.Display.drawString("Ambient TRH Checker", kMarginX, kTitleY);
  drawDeviceInfo();

  M5.Display.setTextSize(2);
  M5.Display.drawString("Time", kColTime, kTableTop);
  M5.Display.drawString("Temp", kColTemp, kTableTop);
  M5.Display.drawString("Hum", kColHum, kTableTop);
  M5.Display.drawString("WiFi", kColWifi, kTableTop);
  M5.Display.drawString("Ambient", kColAmbient, kTableTop);
  M5.Display.drawLine(kMarginX, kTableTop + 30, kTableRight, kTableTop + 30, TFT_BLACK);
  drawWifiStatus();
  drawButton("Reconnect WiFi");
}

void drawStatus() {
  char statusText[80];
  snprintf(statusText, sizeof(statusText), "Read every %lus / Samples %lu",
           static_cast<unsigned long>(kSensorReadIntervalMs / 1000),
           static_cast<unsigned long>(sampleCount));
  M5.Display.fillRect(kMarginX, kStatusY, kTableRight - kMarginX, 40, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.drawString(statusText, kMarginX, kStatusY);
}

void clearRows() {
  const int32_t rowsHeight = static_cast<int32_t>(kVisibleRows) * kRowHeight;
  M5.Display.fillRect(0, kRowsTop, M5.Display.width(), rowsHeight, TFT_WHITE);
  nextDisplayRow = 0;
}

void drawReadingRow(size_t row, const Reading& reading) {
  const int32_t y = kRowsTop + static_cast<int32_t>(row) * kRowHeight;
  M5.Display.fillRect(0, y, M5.Display.width(), kRowHeight, TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char timeText[16];
  char tempText[16];
  char humText[16];
  char ambientText[24];
  formatTime(reading.datetime.time, timeText, sizeof(timeText));
  if (reading.sensorOk) {
    snprintf(tempText, sizeof(tempText), "%.1fC", reading.temperatureC);
    snprintf(humText, sizeof(humText), "%.1f%%", reading.humidityPercent);
  } else {
    snprintf(tempText, sizeof(tempText), "ERR");
    snprintf(humText, sizeof(humText), "ERR");
  }

  if (!reading.ambient.attempted) {
    snprintf(ambientText, sizeof(ambientText), "-");
  } else if (reading.ambient.ok) {
    snprintf(ambientText, sizeof(ambientText), "OK %d", reading.ambient.httpCode);
  } else {
    snprintf(ambientText, sizeof(ambientText), "NG %d", reading.ambient.httpCode);
  }

  M5.Display.drawString(timeText, kColTime, y);
  M5.Display.drawString(tempText, kColTemp, y);
  M5.Display.drawString(humText, kColHum, y);
  M5.Display.drawString(reading.wifiOk ? "OK" : "NG", kColWifi, y);
  M5.Display.drawString(ambientText, kColAmbient, y);
  M5.Display.drawLine(kMarginX, y + 34, kTableRight, y + 34, TFT_BLACK);
}

void refreshDisplay() {
  if (!hasPendingReading || M5.Display.displayBusy()) {
    return;
  }

  bool wrapped = false;
  if (nextDisplayRow >= kVisibleRows) {
    clearRows();
    wrapped = true;
  }

  drawReadingRow(nextDisplayRow, latestReading);
  drawBatteryStatus();
  drawWifiStatus();
  drawStatus();
  ++nextDisplayRow;
  hasPendingReading = false;

  if (wrapped) {
    M5.Display.display(0, kRowsTop, M5.Display.width(), M5.Display.height() - kRowsTop);
  } else {
    const int32_t rowY = kRowsTop + static_cast<int32_t>(nextDisplayRow - 1) * kRowHeight;
    M5.Display.display(0, rowY, M5.Display.width(), kRowHeight);
    M5.Display.display(kMarginX, kBatteryY, kTableRight - kMarginX, 24);
    M5.Display.display(kMarginX, kWifiStatusY, kTableRight - kMarginX, 34);
    M5.Display.display(kMarginX, kStatusY, kTableRight - kMarginX, 40);
  }
}

wl_status_t connectWifi(uint32_t& elapsedMs, bool& timedOut) {
  wifiConnecting = true;
  drawWifiStatus();
  drawButton("Connecting...");
  M5.Display.display(kMarginX, kWifiStatusY, kTableRight - kMarginX, 34);
  M5.Display.display(kButtonX - 2, kButtonY - 2, kButtonW + 4, kButtonH + 4);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < kWifiConnectTimeoutMs) {
    M5.update();
    M5.delay(250);
  }

  elapsedMs = millis() - startedAt;
  const wl_status_t status = WiFi.status();
  timedOut = status != WL_CONNECTED && elapsedMs >= kWifiConnectTimeoutMs;
  wifiConnecting = false;
  wifiLastStatus = status;
  wifiLastElapsedMs = elapsedMs;
  wifiLastTimedOut = timedOut;

  drawWifiStatus();
  drawButton("Reconnect WiFi");
  M5.Display.display(kMarginX, kWifiStatusY, kTableRight - kMarginX, 34);
  M5.Display.display(kButtonX - 2, kButtonY - 2, kButtonW + 4, kButtonH + 4);
  return status;
}

AmbientResult sendAmbient(float temperatureC, float humidityPercent) {
  AmbientResult result;
  result.attempted = true;
  if (WiFi.status() != WL_CONNECTED) {
    result.httpCode = -1;
    return result;
  }
  if (!ambientReady) {
    result.httpCode = -3;
    return result;
  }

  ambient.set(1, static_cast<double>(temperatureC));
  ambient.set(2, static_cast<double>(humidityPercent));
  const bool sent = ambient.send(kAmbientTimeoutMs);
  result.httpCode = ambient.status;
  result.ok = sent && result.httpCode >= 200 && result.httpCode < 300;
  return result;
}

void logReadingToSerial(const Reading& reading) {
  char timeText[16];
  formatTime(reading.datetime.time, timeText, sizeof(timeText));

  Serial.printf("[sample %lu] time=%s ssid=%s ambient_ch=%s sensor=%s",
                static_cast<unsigned long>(sampleCount),
                timeText,
                WIFI_SSID,
                AMBIENT_CHANNEL_ID,
                reading.sensorOk ? "OK" : "ERR");

  if (reading.sensorOk) {
    Serial.printf(" temp=%.2fC hum=%.2f%%",
                  reading.temperatureC,
                  reading.humidityPercent);
  }

  Serial.printf(" wifi=%s", reading.wifiOk ? "OK" : "NG");

  if (reading.ambient.attempted) {
    Serial.printf(" ambient=%s http=%d",
                  reading.ambient.ok ? "OK" : "NG",
                  reading.ambient.httpCode);
  } else {
    Serial.print(" ambient=SKIP");
  }

  Serial.println();
}

void readSensorAndSend() {
  Reading reading;
  M5.Rtc.getDateTime(&reading.datetime);
  reading.sensorOk = readSht30(reading.temperatureC, reading.humidityPercent);
  reading.wifiOk = WiFi.status() == WL_CONNECTED;

  if (reading.sensorOk) {
    reading.ambient = sendAmbient(reading.temperatureC, reading.humidityPercent);
  }

  latestReading = reading;
  hasPendingReading = true;
  ++sampleCount;
  logReadingToSerial(reading);
}
}  // namespace

void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.setRotation(0);
  M5.Display.clear(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawStaticFrame();
  drawStatus();
  M5.Display.display();

  connectWifi(wifiLastElapsedMs, wifiLastTimedOut);
  ambientReady = ambient.begin(static_cast<unsigned int>(atoi(AMBIENT_CHANNEL_ID)),
                               AMBIENT_WRITE_KEY,
                               &ambientClient);
  readSensorAndSend();
  refreshDisplay();

  const uint32_t now = millis();
  lastSensorReadMs = now;
  lastDisplayRefreshMs = now;
}

void loop() {
  M5.update();
  if (!wifiConnecting && isReconnectButtonClicked()) {
    connectWifi(wifiLastElapsedMs, wifiLastTimedOut);
  }

  const uint32_t now = millis();
  if (now - lastSensorReadMs >= kSensorReadIntervalMs) {
    lastSensorReadMs = now;
    readSensorAndSend();
  }
  if (now - lastDisplayRefreshMs >= kDisplayRefreshIntervalMs) {
    lastDisplayRefreshMs = now;
    refreshDisplay();
  }
  M5.delay(10);
}
