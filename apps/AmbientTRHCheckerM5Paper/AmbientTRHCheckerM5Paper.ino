#include <Ambient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <stdlib.h>

#include "secrets.h"

namespace {
namespace Sensor {
constexpr uint8_t kSht30Address = 0x44;
constexpr uint32_t kI2cFrequency = 400000;
constexpr uint32_t kReadIntervalMs = 10000;
constexpr uint8_t kMeasureCommand[] = {0x24, 0x00};
constexpr uint32_t kMeasureDelayMs = 20;
}  // namespace Sensor

namespace Network {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kAmbientTimeoutMs = 5000;
}  // namespace Network

namespace Display {
constexpr uint32_t kRefreshIntervalMs = 1000;
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
}  // namespace Display

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

struct WifiState {
  bool connecting = false;
  bool lastTimedOut = false;
  uint32_t lastElapsedMs = 0;
  wl_status_t lastStatus = WL_IDLE_STATUS;
};

struct AmbientState {
  WiFiClient client;
  Ambient sender;
  bool ready = false;
};

struct AppState {
  uint32_t lastSensorReadMs = 0;
  uint32_t lastDisplayRefreshMs = 0;
  uint32_t sampleCount = 0;
  size_t nextDisplayRow = 0;
  bool hasPendingReading = false;
  Reading latestReading;
};

WifiState wifiState;
AmbientState ambientState;
AppState appState;

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
  uint8_t data[6] = {};

  if (!M5.In_I2C.start(Sensor::kSht30Address, false, Sensor::kI2cFrequency)) {
    return false;
  }

  bool ok = M5.In_I2C.write(Sensor::kMeasureCommand, sizeof(Sensor::kMeasureCommand));
  ok = M5.In_I2C.stop() && ok;
  if (!ok) {
    return false;
  }

  M5.delay(Sensor::kMeasureDelayMs);

  if (!M5.In_I2C.start(Sensor::kSht30Address, true, Sensor::kI2cFrequency)) {
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

bool isReconnectButtonClicked() {
  const auto detail = M5.Touch.getDetail();
  return detail.wasClicked() &&
         detail.x >= Display::kButtonX && detail.x < Display::kButtonX + Display::kButtonW &&
         detail.y >= Display::kButtonY && detail.y < Display::kButtonY + Display::kButtonH;
}

void drawButton(const char* label) {
  M5.Display.fillRect(Display::kButtonX - 2,
                      Display::kButtonY - 2,
                      Display::kButtonW + 4,
                      Display::kButtonH + 4,
                      TFT_WHITE);
  M5.Display.drawRoundRect(Display::kButtonX,
                           Display::kButtonY,
                           Display::kButtonW,
                           Display::kButtonH,
                           8,
                           TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString(label,
                        Display::kButtonX + Display::kButtonW / 2,
                        Display::kButtonY + Display::kButtonH / 2);
  M5.Display.setTextDatum(top_left);
}

void drawWifiStatus() {
  M5.Display.fillRect(Display::kMarginX,
                      Display::kWifiStatusY,
                      Display::kTableRight - Display::kMarginX,
                      34,
                      TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char statusText[96];
  if (wifiState.connecting) {
    snprintf(statusText, sizeof(statusText), "WiFi: connecting to %s", WIFI_SSID);
  } else if (wifiState.lastStatus == WL_CONNECTED) {
    snprintf(statusText,
             sizeof(statusText),
             "WiFi: OK  IP %s  RSSI %d dBm",
             WiFi.localIP().toString().c_str(),
             WiFi.RSSI());
  } else {
    snprintf(statusText,
             sizeof(statusText),
             "WiFi: %s  elapsed %lums%s",
             wifiStatusName(wifiState.lastStatus),
             static_cast<unsigned long>(wifiState.lastElapsedMs),
             wifiState.lastTimedOut ? " timeout" : "");
  }
  M5.Display.drawString(statusText, Display::kMarginX, Display::kWifiStatusY);
}

void drawBatteryStatus() {
  M5.Display.fillRect(Display::kMarginX,
                      Display::kBatteryY,
                      Display::kTableRight - Display::kMarginX,
                      24,
                      TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char batteryText[64];
  const int batteryLevel = M5.Power.getBatteryLevel();
  snprintf(batteryText,
           sizeof(batteryText),
           "Battery: %d%% %s",
           batteryLevel,
           M5.Power.isCharging() ? "charging" : "not charging");
  M5.Display.drawString(batteryText, Display::kMarginX, Display::kBatteryY);
}

void drawDeviceInfo() {
  char ssidText[96];
  char ambientChannelText[64];
  snprintf(ssidText, sizeof(ssidText), "SSID: %s", WIFI_SSID);
  snprintf(ambientChannelText, sizeof(ambientChannelText), "Ambient Ch: %s", AMBIENT_CHANNEL_ID);

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.drawString(ssidText, Display::kMarginX, Display::kSsidY);
  M5.Display.drawString(ambientChannelText, Display::kMarginX, Display::kAmbientChannelY);
  M5.Display.drawString("Ambient fields: d1=temp d2=hum", Display::kMarginX, Display::kAmbientFieldsY);
  drawBatteryStatus();
}

void drawStaticFrame() {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);

  M5.Display.setTextSize(3);
  M5.Display.drawString("Ambient TRH Checker", Display::kMarginX, Display::kTitleY);
  drawDeviceInfo();

  M5.Display.setTextSize(2);
  M5.Display.drawString("Time", Display::kColTime, Display::kTableTop);
  M5.Display.drawString("Temp", Display::kColTemp, Display::kTableTop);
  M5.Display.drawString("Hum", Display::kColHum, Display::kTableTop);
  M5.Display.drawString("WiFi", Display::kColWifi, Display::kTableTop);
  M5.Display.drawString("Ambient", Display::kColAmbient, Display::kTableTop);
  M5.Display.drawLine(Display::kMarginX,
                      Display::kTableTop + 30,
                      Display::kTableRight,
                      Display::kTableTop + 30,
                      TFT_BLACK);
  drawWifiStatus();
  drawButton("Reconnect WiFi");
}

void drawStatus() {
  char statusText[80];
  snprintf(statusText,
           sizeof(statusText),
           "Read every %lus / Samples %lu",
           static_cast<unsigned long>(Sensor::kReadIntervalMs / 1000),
           static_cast<unsigned long>(appState.sampleCount));
  M5.Display.fillRect(Display::kMarginX,
                      Display::kStatusY,
                      Display::kTableRight - Display::kMarginX,
                      40,
                      TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);
  M5.Display.drawString(statusText, Display::kMarginX, Display::kStatusY);
}

void clearRows() {
  const int32_t rowsHeight = static_cast<int32_t>(Display::kVisibleRows) * Display::kRowHeight;
  M5.Display.fillRect(0, Display::kRowsTop, M5.Display.width(), rowsHeight, TFT_WHITE);
  appState.nextDisplayRow = 0;
}

void formatSensorValues(const Reading& reading, char* tempText, size_t tempLength, char* humText, size_t humLength) {
  if (reading.sensorOk) {
    snprintf(tempText, tempLength, "%.1fC", reading.temperatureC);
    snprintf(humText, humLength, "%.1f%%", reading.humidityPercent);
  } else {
    snprintf(tempText, tempLength, "ERR");
    snprintf(humText, humLength, "ERR");
  }
}

void formatAmbientResult(const AmbientResult& result, char* buffer, size_t length) {
  if (!result.attempted) {
    snprintf(buffer, length, "-");
  } else if (result.ok) {
    snprintf(buffer, length, "OK %d", result.httpCode);
  } else {
    snprintf(buffer, length, "NG %d", result.httpCode);
  }
}

void drawReadingRow(size_t row, const Reading& reading) {
  const int32_t y = Display::kRowsTop + static_cast<int32_t>(row) * Display::kRowHeight;
  M5.Display.fillRect(0, y, M5.Display.width(), Display::kRowHeight, TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char timeText[16];
  char tempText[16];
  char humText[16];
  char ambientText[24];
  formatTime(reading.datetime.time, timeText, sizeof(timeText));
  formatSensorValues(reading, tempText, sizeof(tempText), humText, sizeof(humText));
  formatAmbientResult(reading.ambient, ambientText, sizeof(ambientText));

  M5.Display.drawString(timeText, Display::kColTime, y);
  M5.Display.drawString(tempText, Display::kColTemp, y);
  M5.Display.drawString(humText, Display::kColHum, y);
  M5.Display.drawString(reading.wifiOk ? "OK" : "NG", Display::kColWifi, y);
  M5.Display.drawString(ambientText, Display::kColAmbient, y);
  M5.Display.drawLine(Display::kMarginX, y + 34, Display::kTableRight, y + 34, TFT_BLACK);
}

void refreshChangedDisplayRegions(bool wrapped) {
  if (wrapped) {
    M5.Display.display(0, Display::kRowsTop, M5.Display.width(), M5.Display.height() - Display::kRowsTop);
    return;
  }

  const int32_t rowY = Display::kRowsTop + static_cast<int32_t>(appState.nextDisplayRow - 1) * Display::kRowHeight;
  M5.Display.display(0, rowY, M5.Display.width(), Display::kRowHeight);
  M5.Display.display(Display::kMarginX, Display::kBatteryY, Display::kTableRight - Display::kMarginX, 24);
  M5.Display.display(Display::kMarginX, Display::kWifiStatusY, Display::kTableRight - Display::kMarginX, 34);
  M5.Display.display(Display::kMarginX, Display::kStatusY, Display::kTableRight - Display::kMarginX, 40);
}

void refreshDisplay() {
  if (!appState.hasPendingReading || M5.Display.displayBusy()) {
    return;
  }

  bool wrapped = false;
  if (appState.nextDisplayRow >= Display::kVisibleRows) {
    clearRows();
    wrapped = true;
  }

  drawReadingRow(appState.nextDisplayRow, appState.latestReading);
  drawBatteryStatus();
  drawWifiStatus();
  drawStatus();
  ++appState.nextDisplayRow;
  appState.hasPendingReading = false;

  refreshChangedDisplayRegions(wrapped);
}

void refreshWifiUi(const char* buttonLabel) {
  drawWifiStatus();
  drawButton(buttonLabel);
  M5.Display.display(Display::kMarginX, Display::kWifiStatusY, Display::kTableRight - Display::kMarginX, 34);
  M5.Display.display(Display::kButtonX - 2,
                     Display::kButtonY - 2,
                     Display::kButtonW + 4,
                     Display::kButtonH + 4);
}

wl_status_t connectWifi(uint32_t& elapsedMs, bool& timedOut) {
  wifiState.connecting = true;
  refreshWifiUi("Connecting...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < Network::kWifiConnectTimeoutMs) {
    M5.update();
    M5.delay(250);
  }

  elapsedMs = millis() - startedAt;
  const wl_status_t status = WiFi.status();
  timedOut = status != WL_CONNECTED && elapsedMs >= Network::kWifiConnectTimeoutMs;

  wifiState.connecting = false;
  wifiState.lastStatus = status;
  wifiState.lastElapsedMs = elapsedMs;
  wifiState.lastTimedOut = timedOut;
  refreshWifiUi("Reconnect WiFi");

  return status;
}

AmbientResult sendAmbient(float temperatureC, float humidityPercent) {
  AmbientResult result;
  result.attempted = true;

  if (WiFi.status() != WL_CONNECTED) {
    result.httpCode = -1;
    return result;
  }
  if (!ambientState.ready) {
    result.httpCode = -3;
    return result;
  }

  ambientState.sender.set(1, static_cast<double>(temperatureC));
  ambientState.sender.set(2, static_cast<double>(humidityPercent));
  const bool sent = ambientState.sender.send(Network::kAmbientTimeoutMs);
  result.httpCode = ambientState.sender.status;
  result.ok = sent && result.httpCode >= 200 && result.httpCode < 300;
  return result;
}

void logReadingToSerial(const Reading& reading) {
  char timeText[16];
  formatTime(reading.datetime.time, timeText, sizeof(timeText));

  Serial.printf("[sample %lu] time=%s ssid=%s ambient_ch=%s sensor=%s",
                static_cast<unsigned long>(appState.sampleCount),
                timeText,
                WIFI_SSID,
                AMBIENT_CHANNEL_ID,
                reading.sensorOk ? "OK" : "ERR");

  if (reading.sensorOk) {
    Serial.printf(" temp=%.2fC hum=%.2f%%", reading.temperatureC, reading.humidityPercent);
  }

  Serial.printf(" wifi=%s", reading.wifiOk ? "OK" : "NG");

  if (reading.ambient.attempted) {
    Serial.printf(" ambient=%s http=%d", reading.ambient.ok ? "OK" : "NG", reading.ambient.httpCode);
  } else {
    Serial.print(" ambient=SKIP");
  }

  Serial.println();
}

Reading takeReading() {
  Reading reading;
  M5.Rtc.getDateTime(&reading.datetime);
  reading.sensorOk = readSht30(reading.temperatureC, reading.humidityPercent);
  reading.wifiOk = WiFi.status() == WL_CONNECTED;

  if (reading.sensorOk) {
    reading.ambient = sendAmbient(reading.temperatureC, reading.humidityPercent);
  }

  return reading;
}

void sampleAndSend() {
  appState.latestReading = takeReading();
  appState.hasPendingReading = true;
  ++appState.sampleCount;
  logReadingToSerial(appState.latestReading);
}

void initializeDisplay() {
  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.setRotation(0);
  M5.Display.clear(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawStaticFrame();
  drawStatus();
  M5.Display.display();
}

void initializeAmbient() {
  ambientState.ready = ambientState.sender.begin(static_cast<unsigned int>(atoi(AMBIENT_CHANNEL_ID)),
                                                 AMBIENT_WRITE_KEY,
                                                 &ambientState.client);
}

void recordLoopTimestamps(uint32_t now) {
  appState.lastSensorReadMs = now;
  appState.lastDisplayRefreshMs = now;
}
}  // namespace

void setup() {
  M5.begin();
  Serial.begin(115200);

  initializeDisplay();
  connectWifi(wifiState.lastElapsedMs, wifiState.lastTimedOut);
  initializeAmbient();
  sampleAndSend();
  refreshDisplay();
  recordLoopTimestamps(millis());
}

void loop() {
  M5.update();

  if (!wifiState.connecting && isReconnectButtonClicked()) {
    connectWifi(wifiState.lastElapsedMs, wifiState.lastTimedOut);
  }

  const uint32_t now = millis();
  if (now - appState.lastSensorReadMs >= Sensor::kReadIntervalMs) {
    appState.lastSensorReadMs = now;
    sampleAndSend();
  }
  if (now - appState.lastDisplayRefreshMs >= Display::kRefreshIntervalMs) {
    appState.lastDisplayRefreshMs = now;
    refreshDisplay();
  }

  M5.delay(10);
}
