#include <Ambient.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "secrets.h"

namespace {
namespace Sensor {
constexpr uint8_t kPumpPin = 26;
constexpr uint8_t kMoisturePin = 33;
constexpr uint8_t kSht30Address = 0x44;
constexpr uint32_t kI2cFrequency = 400000;
constexpr uint32_t kReadIntervalMs = 10000;
constexpr uint32_t kPumpRunMs = 5000;
constexpr uint16_t kAdcMax = 4095;
constexpr uint8_t kMeasureCommand[] = {0x24, 0x00};
constexpr uint32_t kMeasureDelayMs = 20;
}  // namespace Sensor

namespace Network {
constexpr long kJstOffsetSec = 9 * 60 * 60;
constexpr int kDaylightOffsetSec = 0;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kNtpTimeoutMs = 15000;
constexpr uint32_t kAmbientTimeoutMs = 5000;
constexpr uint32_t kAmbientSendIntervalMs = 60000;
constexpr int kMinimumValidYear = 2024;
constexpr const char* kNtpServer1 = "ntp.nict.jp";
constexpr const char* kNtpServer2 = "ntp.jst.mfeed.ad.jp";
}  // namespace Network

namespace Display {
constexpr uint32_t kRefreshIntervalMs = 1000;
constexpr int32_t kMarginX = 18;
constexpr int32_t kTitleY = 22;
constexpr int32_t kHeaderIdentityY = 58;
constexpr int32_t kHeaderStatsY = 90;
constexpr int32_t kHeaderPumpY = 174;
constexpr int32_t kHeaderStatusY = 198;
constexpr int32_t kHeaderBottom = 232;
constexpr int32_t kTableTop = 254;
constexpr int32_t kRowsTop = kTableTop + 42;
constexpr int32_t kRowHeight = 48;
constexpr size_t kVisibleRows = 10;
constexpr int32_t kColTime = 18;
constexpr int32_t kColMoist = 124;
constexpr int32_t kColTemp = 230;
constexpr int32_t kColHum = 326;
constexpr int32_t kColAmbient = 426;
constexpr int32_t kTableRight = 526;
constexpr int32_t kPumpButtonX = 28;
constexpr int32_t kWifiButtonX = 280;
constexpr int32_t kButtonY = 842;
constexpr int32_t kButtonW = 232;
constexpr int32_t kButtonH = 78;
constexpr size_t kLogQueueSize = 8;
}  // namespace Display

enum class LogKind {
  Reading,
  PumpStarted,
};

struct LogEntry {
  LogKind kind = LogKind::Reading;
  uint32_t sample = 0;
  uint32_t elapsedSec = 0;
  bool timeOk = false;
  m5::rtc_datetime_t datetime;
  uint16_t moist = 0;
  uint32_t millivolts = 0;
  uint16_t minMoist = Sensor::kAdcMax;
  uint16_t maxMoist = 0;
  float moistAdcPercent = 0.0f;
  bool environmentOk = false;
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
  bool ambientAttempted = false;
  bool ambientOk = false;
  int ambientHttpCode = 0;
};

struct AmbientResult {
  bool attempted = false;
  bool ok = false;
  int httpCode = 0;
};

struct AppState {
  uint32_t lastSensorReadMs = 0;
  uint32_t lastDisplayRefreshMs = 0;
  uint32_t lastAmbientSendMs = 0;
  uint32_t pumpStartedMs = 0;
  uint32_t sampleCount = 0;
  size_t nextDisplayRow = 0;
  bool pumpRunning = false;
  bool pumpUiDirty = false;
  bool headerDirty = false;
  bool wifiOk = false;
  bool ntpOk = false;
  bool ambientReady = false;
  uint32_t wifiElapsedMs = 0;
  uint32_t ntpElapsedMs = 0;
  uint32_t pumpCount = 0;
  bool hasLastPumpTime = false;
  m5::rtc_datetime_t lastPumpDateTime;
  uint32_t lastPumpElapsedSec = 0;
  bool hasEnvironmentStats = false;
  float minTemperatureC = 0.0f;
  float maxTemperatureC = 0.0f;
  float minHumidityPercent = 0.0f;
  float maxHumidityPercent = 0.0f;
  LogEntry latestReading;
  LogEntry logQueue[Display::kLogQueueSize];
  size_t logQueueHead = 0;
  size_t logQueueCount = 0;
};

AppState appState;
WiFiClient ambientClient;
Ambient ambientSender;

float moistToAdcPercent(uint16_t moist) {
  return static_cast<float>(moist) * 100.0f / static_cast<float>(Sensor::kAdcMax);
}

void formatTime(const m5::rtc_time_t& time, char* buffer, size_t length) {
  snprintf(buffer, length, "%02d:%02d:%02d", time.hours, time.minutes, time.seconds);
}

void formatLogTime(bool timeOk,
                   const m5::rtc_datetime_t& datetime,
                   uint32_t elapsedSec,
                   char* buffer,
                   size_t length) {
  if (timeOk) {
    formatTime(datetime.time, buffer, length);
  } else {
    snprintf(buffer, length, "%lus", static_cast<unsigned long>(elapsedSec));
  }
}

bool isValidNtpTime(const tm& timeinfo) {
  return timeinfo.tm_year + 1900 >= Network::kMinimumValidYear;
}

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

bool isButtonClicked(const m5::touch_detail_t& detail, int32_t x, int32_t y, int32_t w, int32_t h) {
  return detail.wasClicked() &&
         detail.x >= x && detail.x < x + w &&
         detail.y >= y && detail.y < y + h;
}

void drawButton(int32_t x, const char* label) {
  M5.Display.fillRect(x - 3,
                      Display::kButtonY - 3,
                      Display::kButtonW + 6,
                      Display::kButtonH + 6,
                      TFT_WHITE);
  M5.Display.drawRoundRect(x,
                           Display::kButtonY,
                           Display::kButtonW,
                           Display::kButtonH,
                           8,
                           TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString(label,
                        x + Display::kButtonW / 2,
                        Display::kButtonY + Display::kButtonH / 2);
  M5.Display.setTextDatum(top_left);
}

void drawButtons() {
  drawButton(Display::kPumpButtonX, appState.pumpRunning ? "Watering" : "Pump");
  drawButton(Display::kWifiButtonX, "Reconnect");
}

void drawHeader() {
  M5.Display.fillRect(0, 0, M5.Display.width(), Display::kHeaderBottom, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

  M5.Display.setTextSize(3);
  M5.Display.drawString("Watering Unit Checker", Display::kMarginX, Display::kTitleY);

  M5.Display.setTextSize(2);
  char identityText[128];
  snprintf(identityText,
           sizeof(identityText),
           "SSID: %s / Ambient Ch: %s",
           WIFI_SSID,
           AMBIENT_CHANNEL_ID);
  M5.Display.drawString(identityText, Display::kMarginX, Display::kHeaderIdentityY);

  char moistText[64];
  if (appState.latestReading.sample == 0) {
    snprintf(moistText, sizeof(moistText), "Moist min/max: - / -");
  } else {
    snprintf(moistText,
             sizeof(moistText),
             "Moist min/max: %u / %u",
             appState.latestReading.minMoist,
             appState.latestReading.maxMoist);
  }

  char tempText[80];
  char humText[80];
  if (appState.hasEnvironmentStats) {
    snprintf(tempText,
             sizeof(tempText),
             "Temp min/max: %.1fC / %.1fC",
             appState.minTemperatureC,
             appState.maxTemperatureC);
    snprintf(humText,
             sizeof(humText),
             "Hum min/max: %.1f%% / %.1f%%",
             appState.minHumidityPercent,
             appState.maxHumidityPercent);
  } else {
    snprintf(tempText, sizeof(tempText), "Temp min/max: - / -");
    snprintf(humText, sizeof(humText), "Hum min/max: - / -");
  }

  M5.Display.drawString(moistText, Display::kMarginX, Display::kHeaderStatsY);
  M5.Display.drawString(tempText, Display::kMarginX, Display::kHeaderStatsY + 28);
  M5.Display.drawString(humText, Display::kMarginX, Display::kHeaderStatsY + 56);

  char lastPumpTimeText[24];
  if (appState.hasLastPumpTime) {
    formatLogTime(true,
                  appState.lastPumpDateTime,
                  appState.lastPumpElapsedSec,
                  lastPumpTimeText,
                  sizeof(lastPumpTimeText));
  } else {
    snprintf(lastPumpTimeText, sizeof(lastPumpTimeText), "-");
  }

  char pumpSummaryText[96];
  snprintf(pumpSummaryText,
           sizeof(pumpSummaryText),
           "Last pump: %s / Count: %lu",
           lastPumpTimeText,
           static_cast<unsigned long>(appState.pumpCount));
  M5.Display.drawString(pumpSummaryText, Display::kMarginX, Display::kHeaderPumpY);

  char statusText[128];
  if (appState.pumpRunning) {
    const uint32_t elapsedMs = millis() - appState.pumpStartedMs;
    const uint32_t remainingMs = elapsedMs >= Sensor::kPumpRunMs ? 0 : Sensor::kPumpRunMs - elapsedMs;
    snprintf(statusText,
             sizeof(statusText),
             "WiFi %s / NTP %s / Pump ON 5s rem %lus",
             appState.wifiOk ? "OK" : "NG",
             appState.ntpOk ? "OK" : "NG",
             static_cast<unsigned long>((remainingMs + 999) / 1000));
  } else {
    snprintf(statusText,
             sizeof(statusText),
             "WiFi %s / NTP %s / Pump OFF 5s",
             appState.wifiOk ? "OK" : "NG",
             appState.ntpOk ? "OK" : "NG");
  }
  M5.Display.drawString(statusText, Display::kMarginX, Display::kHeaderStatusY);
  M5.Display.drawLine(Display::kMarginX, Display::kHeaderBottom - 1, Display::kTableRight, Display::kHeaderBottom - 1, TFT_BLACK);
}

void drawStaticFrame() {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);

  drawHeader();

  M5.Display.drawString("Time", Display::kColTime, Display::kTableTop);
  M5.Display.drawString("Moist", Display::kColMoist, Display::kTableTop);
  M5.Display.drawString("Temp", Display::kColTemp, Display::kTableTop);
  M5.Display.drawString("Hum", Display::kColHum, Display::kTableTop);
  M5.Display.drawString("Amb", Display::kColAmbient, Display::kTableTop);
  M5.Display.drawLine(Display::kMarginX,
                      Display::kTableTop + 30,
                      Display::kTableRight,
                      Display::kTableTop + 30,
                      TFT_BLACK);
  drawButtons();
}

void clearRows() {
  const int32_t rowsHeight = static_cast<int32_t>(Display::kVisibleRows) * Display::kRowHeight;
  M5.Display.fillRect(0, Display::kRowsTop, M5.Display.width(), rowsHeight, TFT_WHITE);
  appState.nextDisplayRow = 0;
}

void drawLogRow(size_t row, const LogEntry& entry) {
  const int32_t y = Display::kRowsTop + static_cast<int32_t>(row) * Display::kRowHeight;
  M5.Display.fillRect(0, y, M5.Display.width(), Display::kRowHeight, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char timeText[24];
  formatLogTime(entry.timeOk, entry.datetime, entry.elapsedSec, timeText, sizeof(timeText));
  M5.Display.drawString(timeText, Display::kColTime, y);

  if (entry.kind == LogKind::Reading) {
    char moistText[16];
    char tempText[16];
    char humText[16];
    char ambientText[16];
    snprintf(moistText, sizeof(moistText), "%u", entry.moist);
    if (entry.environmentOk) {
      snprintf(tempText, sizeof(tempText), "%.1fC", entry.temperatureC);
      snprintf(humText, sizeof(humText), "%.1f%%", entry.humidityPercent);
    } else {
      snprintf(tempText, sizeof(tempText), "ERR");
      snprintf(humText, sizeof(humText), "ERR");
    }
    if (entry.ambientAttempted) {
      snprintf(ambientText, sizeof(ambientText), entry.ambientOk ? "OK" : "NG");
    } else {
      snprintf(ambientText, sizeof(ambientText), "SKIP");
    }

    M5.Display.drawString(moistText, Display::kColMoist, y);
    M5.Display.drawString(tempText, Display::kColTemp, y);
    M5.Display.drawString(humText, Display::kColHum, y);
    M5.Display.drawString(ambientText, Display::kColAmbient, y);
  } else {
    M5.Display.drawString("PUMP ON", Display::kColMoist, y);
  }

  M5.Display.drawLine(Display::kMarginX, y + 34, Display::kTableRight, y + 34, TFT_BLACK);
}

void queueLogEntry(const LogEntry& entry) {
  const size_t index = (appState.logQueueHead + appState.logQueueCount) % Display::kLogQueueSize;
  appState.logQueue[index] = entry;
  if (appState.logQueueCount < Display::kLogQueueSize) {
    ++appState.logQueueCount;
  } else {
    appState.logQueueHead = (appState.logQueueHead + 1) % Display::kLogQueueSize;
  }
}

bool popLogEntry(LogEntry& entry) {
  if (appState.logQueueCount == 0) {
    return false;
  }

  entry = appState.logQueue[appState.logQueueHead];
  appState.logQueueHead = (appState.logQueueHead + 1) % Display::kLogQueueSize;
  --appState.logQueueCount;
  return true;
}

LogEntry makeEventLog(LogKind kind) {
  LogEntry entry;
  entry.kind = kind;
  entry.elapsedSec = millis() / 1000;
  entry.timeOk = appState.ntpOk && M5.Rtc.getDateTime(&entry.datetime);
  return entry;
}

void initializeAmbient() {
  appState.ambientReady = ambientSender.begin(static_cast<unsigned int>(atoi(AMBIENT_CHANNEL_ID)),
                                              AMBIENT_WRITE_KEY,
                                              &ambientClient);
  Serial.printf("Ambient: %s channel=%s\n", appState.ambientReady ? "READY" : "NG", AMBIENT_CHANNEL_ID);
}

AmbientResult sendAmbient(const LogEntry& reading) {
  AmbientResult result;

  if (!reading.environmentOk) {
    result.httpCode = -4;
    return result;
  }
  if (WiFi.status() != WL_CONNECTED) {
    result.httpCode = -1;
    return result;
  }
  if (!appState.ambientReady) {
    result.httpCode = -3;
    return result;
  }

  result.attempted = true;
  ambientSender.set(1, static_cast<double>(reading.moist));
  ambientSender.set(2, static_cast<double>(reading.temperatureC));
  ambientSender.set(3, static_cast<double>(reading.humidityPercent));
  const bool sent = ambientSender.send(Network::kAmbientTimeoutMs);
  result.httpCode = ambientSender.status;
  result.ok = sent && result.httpCode >= 200 && result.httpCode < 300;
  return result;
}

void readMoisture(uint32_t now) {
  ++appState.sampleCount;
  LogEntry reading;
  reading.kind = LogKind::Reading;
  reading.sample = appState.sampleCount;
  reading.elapsedSec = millis() / 1000;
  reading.timeOk = appState.ntpOk && M5.Rtc.getDateTime(&reading.datetime);
  reading.moist = static_cast<uint16_t>(analogRead(Sensor::kMoisturePin));
  reading.millivolts = analogReadMilliVolts(Sensor::kMoisturePin);
  reading.environmentOk = readSht30(reading.temperatureC, reading.humidityPercent);
  const bool moistStatsChanged = appState.latestReading.sample == 0 ||
                               reading.moist < appState.latestReading.minMoist ||
                               reading.moist > appState.latestReading.maxMoist;
  reading.minMoist = appState.latestReading.sample == 0 ? reading.moist : min(appState.latestReading.minMoist, reading.moist);
  reading.maxMoist = appState.latestReading.sample == 0 ? reading.moist : max(appState.latestReading.maxMoist, reading.moist);
  reading.moistAdcPercent = moistToAdcPercent(reading.moist);

  bool environmentStatsChanged = false;
  if (reading.environmentOk) {
    if (!appState.hasEnvironmentStats) {
      appState.minTemperatureC = reading.temperatureC;
      appState.maxTemperatureC = reading.temperatureC;
      appState.minHumidityPercent = reading.humidityPercent;
      appState.maxHumidityPercent = reading.humidityPercent;
      appState.hasEnvironmentStats = true;
      environmentStatsChanged = true;
    } else {
      environmentStatsChanged = reading.temperatureC < appState.minTemperatureC ||
                                reading.temperatureC > appState.maxTemperatureC ||
                                reading.humidityPercent < appState.minHumidityPercent ||
                                reading.humidityPercent > appState.maxHumidityPercent;
      appState.minTemperatureC = min(appState.minTemperatureC, reading.temperatureC);
      appState.maxTemperatureC = max(appState.maxTemperatureC, reading.temperatureC);
      appState.minHumidityPercent = min(appState.minHumidityPercent, reading.humidityPercent);
      appState.maxHumidityPercent = max(appState.maxHumidityPercent, reading.humidityPercent);
    }
  }

  if (moistStatsChanged || environmentStatsChanged) {
    appState.headerDirty = true;
  }

  const bool shouldSendAmbient = now - appState.lastAmbientSendMs >= Network::kAmbientSendIntervalMs;
  AmbientResult ambient;
  if (shouldSendAmbient) {
    ambient = sendAmbient(reading);
    appState.lastAmbientSendMs = now;
  }
  reading.ambientAttempted = ambient.attempted;
  reading.ambientOk = ambient.ok;
  reading.ambientHttpCode = ambient.httpCode;

  appState.latestReading = reading;
  queueLogEntry(reading);

  char timeText[24];
  if (reading.timeOk) {
    formatTime(reading.datetime.time, timeText, sizeof(timeText));
  } else {
    snprintf(timeText, sizeof(timeText), "%lus", static_cast<unsigned long>(reading.elapsedSec));
  }

  Serial.printf("[sample %lu] time=%s elapsed=%lus moist=%u moist_mv=%lu adc_percent=%.1f min_moist=%u max_moist=%u%s env=%s",
                static_cast<unsigned long>(reading.sample),
                timeText,
                static_cast<unsigned long>(reading.elapsedSec),
                reading.moist,
                static_cast<unsigned long>(reading.millivolts),
                reading.moistAdcPercent,
                reading.minMoist,
                reading.maxMoist,
                reading.moist >= 4090 ? " saturated" : "",
                reading.environmentOk ? "OK" : "ERR");
  if (reading.environmentOk) {
    Serial.printf(" temp_c=%.2f humidity_percent=%.2f", reading.temperatureC, reading.humidityPercent);
  }
  if (reading.ambientAttempted) {
    Serial.printf(" ambient=%s http=%d", reading.ambientOk ? "OK" : "NG", reading.ambientHttpCode);
  } else {
    Serial.printf(" ambient=SKIP%s", shouldSendAmbient ? " blocked" : "");
  }
  Serial.printf(" pump=%s\n", appState.pumpRunning ? "ON" : "OFF");
}

void refreshDisplay() {
  if (M5.Display.displayBusy()) {
    return;
  }

  bool updated = false;
  bool headerUpdated = false;
  bool buttonsUpdated = false;
  bool wrapped = false;

  LogEntry entry;
  int32_t firstUpdatedRowY = -1;
  int32_t lastUpdatedRowY = -1;
  while (popLogEntry(entry)) {
    if (appState.nextDisplayRow >= Display::kVisibleRows) {
      clearRows();
      wrapped = true;
    }

    const int32_t rowY = Display::kRowsTop + static_cast<int32_t>(appState.nextDisplayRow) * Display::kRowHeight;
    drawLogRow(appState.nextDisplayRow, entry);
    if (firstUpdatedRowY < 0) {
      firstUpdatedRowY = rowY;
    }
    lastUpdatedRowY = rowY;
    ++appState.nextDisplayRow;
    updated = true;
  }

  if (appState.headerDirty) {
    drawHeader();
    appState.headerDirty = false;
    headerUpdated = true;
  }

  if (appState.pumpUiDirty) {
    drawButtons();
    appState.pumpUiDirty = false;
    buttonsUpdated = true;
  }

  if (!updated && !headerUpdated && !buttonsUpdated) {
    return;
  }

  if (wrapped) {
    M5.Display.display(0, Display::kRowsTop, M5.Display.width(), Display::kButtonY - Display::kRowsTop);
    if (headerUpdated) {
      M5.Display.display(0, 0, M5.Display.width(), Display::kHeaderBottom);
    }
    if (buttonsUpdated) {
      M5.Display.display(Display::kPumpButtonX - 3,
                         Display::kButtonY - 3,
                         Display::kWifiButtonX + Display::kButtonW - Display::kPumpButtonX + 6,
                         Display::kButtonH + 6);
    }
    return;
  }

  if (headerUpdated) {
    M5.Display.display(0, 0, M5.Display.width(), Display::kHeaderBottom);
  }
  if (firstUpdatedRowY >= 0) {
    M5.Display.display(0,
                       firstUpdatedRowY,
                       M5.Display.width(),
                       lastUpdatedRowY - firstUpdatedRowY + Display::kRowHeight);
  }
  if (buttonsUpdated) {
    M5.Display.display(Display::kPumpButtonX - 3,
                       Display::kButtonY - 3,
                       Display::kWifiButtonX + Display::kButtonW - Display::kPumpButtonX + 6,
                       Display::kButtonH + 6);
  }
}

void startPump() {
  if (appState.pumpRunning) {
    return;
  }

  digitalWrite(Sensor::kPumpPin, HIGH);
  appState.pumpRunning = true;
  appState.pumpStartedMs = millis();
  ++appState.pumpCount;
  appState.lastPumpElapsedSec = appState.pumpStartedMs / 1000;
  appState.hasLastPumpTime = M5.Rtc.getDateTime(&appState.lastPumpDateTime);
  queueLogEntry(makeEventLog(LogKind::PumpStarted));
  appState.headerDirty = true;
  appState.pumpUiDirty = true;
  Serial.println("Pump started for 5 seconds");
}

void stopPump() {
  if (!appState.pumpRunning) {
    return;
  }

  digitalWrite(Sensor::kPumpPin, LOW);
  appState.pumpRunning = false;
  appState.headerDirty = true;
  appState.pumpUiDirty = true;
  Serial.println("Pump stopped");
}

void updatePump(uint32_t now) {
  if (appState.pumpRunning && now - appState.pumpStartedMs >= Sensor::kPumpRunMs) {
    stopPump();
  }
}

void initializeDisplay() {
  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.setRotation(0);
  M5.Display.clear(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawStaticFrame();
  M5.Display.display();
}

void refreshHeaderAndButtons() {
  if (M5.Display.displayBusy()) {
    return;
  }

  drawHeader();
  drawButtons();
  appState.headerDirty = false;
  M5.Display.display(0, 0, M5.Display.width(), Display::kHeaderBottom);
  M5.Display.display(Display::kPumpButtonX - 3,
                     Display::kButtonY - 3,
                     Display::kWifiButtonX + Display::kButtonW - Display::kPumpButtonX + 6,
                     Display::kButtonH + 6);
}

void connectWifi() {
  appState.wifiOk = false;
  appState.ntpOk = false;
  appState.headerDirty = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < Network::kWifiConnectTimeoutMs) {
    M5.update();
    M5.delay(250);
  }

  appState.wifiElapsedMs = millis() - startedAt;
  appState.wifiOk = WiFi.status() == WL_CONNECTED;
  Serial.printf("WiFi: %s elapsed=%lums\n",
                appState.wifiOk ? "OK" : "NG",
                static_cast<unsigned long>(appState.wifiElapsedMs));
  appState.headerDirty = true;
}

void syncTimeFromNtp() {
  if (!appState.wifiOk) {
    appState.ntpOk = false;
    return;
  }

  timeval resetTime = {};
  settimeofday(&resetTime, nullptr);
  configTime(Network::kJstOffsetSec,
             Network::kDaylightOffsetSec,
             Network::kNtpServer1,
             Network::kNtpServer2);

  tm timeinfo;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < Network::kNtpTimeoutMs) {
    if (getLocalTime(&timeinfo, 250) && isValidNtpTime(timeinfo)) {
      break;
    }
    M5.update();
    M5.delay(250);
  }

  appState.ntpElapsedMs = millis() - startedAt;
  appState.ntpOk = getLocalTime(&timeinfo, 250) && isValidNtpTime(timeinfo);
  if (appState.ntpOk) {
    M5.Rtc.setDateTime(&timeinfo);
    Serial.printf("NTP: OK %04d/%02d/%02d %02d:%02d:%02d elapsed=%lums\n",
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_mday,
                  timeinfo.tm_hour,
                  timeinfo.tm_min,
                  timeinfo.tm_sec,
                  static_cast<unsigned long>(appState.ntpElapsedMs));
  } else {
    Serial.printf("NTP: NG elapsed=%lums\n", static_cast<unsigned long>(appState.ntpElapsedMs));
  }
  appState.headerDirty = true;
}

void reconnectWifiAndSyncTime() {
  connectWifi();
  syncTimeFromNtp();
  initializeAmbient();
  refreshHeaderAndButtons();
}
}  // namespace

void setup() {
  M5.begin();
  Serial.begin(115200);

  pinMode(Sensor::kPumpPin, OUTPUT);
  digitalWrite(Sensor::kPumpPin, LOW);
  pinMode(Sensor::kMoisturePin, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(Sensor::kMoisturePin, ADC_11db);

  initializeDisplay();
  connectWifi();
  syncTimeFromNtp();
  initializeAmbient();
  refreshHeaderAndButtons();
  const uint32_t now = millis();
  appState.lastAmbientSendMs = now - Network::kAmbientSendIntervalMs;
  readMoisture(now);
  refreshDisplay();

  appState.lastSensorReadMs = now;
  appState.lastDisplayRefreshMs = now;
}

void loop() {
  M5.update();

  const auto touch = M5.Touch.getDetail();
  if (isButtonClicked(touch,
                      Display::kPumpButtonX,
                      Display::kButtonY,
                      Display::kButtonW,
                      Display::kButtonH)) {
    startPump();
  } else if (isButtonClicked(touch,
                             Display::kWifiButtonX,
                             Display::kButtonY,
                             Display::kButtonW,
                             Display::kButtonH)) {
    reconnectWifiAndSyncTime();
  }

  const uint32_t now = millis();
  updatePump(now);

  if (now - appState.lastSensorReadMs >= Sensor::kReadIntervalMs) {
    appState.lastSensorReadMs = now;
    readMoisture(now);
  }

  if (now - appState.lastDisplayRefreshMs >= Display::kRefreshIntervalMs) {
    appState.lastDisplayRefreshMs = now;
    refreshDisplay();
  }

  M5.delay(10);
}
