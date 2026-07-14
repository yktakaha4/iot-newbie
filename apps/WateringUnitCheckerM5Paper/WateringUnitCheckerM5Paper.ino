#include <M5Unified.h>
#include <WiFi.h>
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
constexpr uint16_t kWetRaw = 1600;
constexpr uint16_t kDryRaw = 2000;
constexpr uint8_t kMeasureCommand[] = {0x24, 0x00};
constexpr uint32_t kMeasureDelayMs = 20;
}  // namespace Sensor

namespace Network {
constexpr long kJstOffsetSec = 9 * 60 * 60;
constexpr int kDaylightOffsetSec = 0;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kNtpTimeoutMs = 15000;
constexpr int kMinimumValidYear = 2024;
constexpr const char* kNtpServer1 = "ntp.nict.jp";
constexpr const char* kNtpServer2 = "ntp.jst.mfeed.ad.jp";
}  // namespace Network

namespace Display {
constexpr uint32_t kRefreshIntervalMs = 1000;
constexpr int32_t kMarginX = 18;
constexpr int32_t kTitleY = 22;
constexpr int32_t kHeaderStatsY = 66;
constexpr int32_t kHeaderStatusY = 150;
constexpr int32_t kHeaderBottom = 188;
constexpr int32_t kTableTop = 216;
constexpr int32_t kRowsTop = kTableTop + 42;
constexpr int32_t kRowHeight = 48;
constexpr size_t kVisibleRows = 12;
constexpr int32_t kColTime = 18;
constexpr int32_t kColRaw = 136;
constexpr int32_t kColPercent = 236;
constexpr int32_t kColTemp = 340;
constexpr int32_t kColHum = 438;
constexpr int32_t kTableRight = 526;
constexpr int32_t kPumpButtonX = 28;
constexpr int32_t kWifiButtonX = 280;
constexpr int32_t kButtonY = 842;
constexpr int32_t kButtonW = 232;
constexpr int32_t kButtonH = 78;
}  // namespace Display

struct Reading {
  uint32_t sample = 0;
  uint32_t elapsedSec = 0;
  bool timeOk = false;
  m5::rtc_datetime_t datetime;
  uint16_t raw = 0;
  uint32_t millivolts = 0;
  uint16_t minRaw = Sensor::kAdcMax;
  uint16_t maxRaw = 0;
  float adcPercent = 0.0f;
  float moisturePercent = 0.0f;
  bool environmentOk = false;
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
};

struct AppState {
  uint32_t lastSensorReadMs = 0;
  uint32_t lastDisplayRefreshMs = 0;
  uint32_t pumpStartedMs = 0;
  uint32_t sampleCount = 0;
  size_t nextDisplayRow = 0;
  bool pumpRunning = false;
  bool hasPendingReading = false;
  bool pumpUiDirty = false;
  bool headerDirty = false;
  bool wifiOk = false;
  bool ntpOk = false;
  uint32_t wifiElapsedMs = 0;
  uint32_t ntpElapsedMs = 0;
  bool hasEnvironmentStats = false;
  float minTemperatureC = 0.0f;
  float maxTemperatureC = 0.0f;
  float minHumidityPercent = 0.0f;
  float maxHumidityPercent = 0.0f;
  Reading latestReading;
};

AppState appState;

float rawToAdcPercent(uint16_t raw) {
  return static_cast<float>(raw) * 100.0f / static_cast<float>(Sensor::kAdcMax);
}

float rawToMoisturePercent(uint16_t raw) {
  if (Sensor::kDryRaw == Sensor::kWetRaw) {
    return 0.0f;
  }

  const float percent = (static_cast<float>(Sensor::kDryRaw) - static_cast<float>(raw)) * 100.0f /
                        (static_cast<float>(Sensor::kDryRaw) - static_cast<float>(Sensor::kWetRaw));
  return constrain(percent, 0.0f, 100.0f);
}

void formatTime(const m5::rtc_time_t& time, char* buffer, size_t length) {
  snprintf(buffer, length, "%02d:%02d:%02d", time.hours, time.minutes, time.seconds);
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

  char rawText[64];
  if (appState.latestReading.sample == 0) {
    snprintf(rawText, sizeof(rawText), "Raw min/max: - / -");
  } else {
    snprintf(rawText,
             sizeof(rawText),
             "Raw min/max: %u / %u",
             appState.latestReading.minRaw,
             appState.latestReading.maxRaw);
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

  M5.Display.setTextSize(2);
  M5.Display.drawString(rawText, Display::kMarginX, Display::kHeaderStatsY);
  M5.Display.drawString(tempText, Display::kMarginX, Display::kHeaderStatsY + 28);
  M5.Display.drawString(humText, Display::kMarginX, Display::kHeaderStatsY + 56);

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
  M5.Display.drawString("Raw", Display::kColRaw, Display::kTableTop);
  M5.Display.drawString("Moist", Display::kColPercent, Display::kTableTop);
  M5.Display.drawString("Temp", Display::kColTemp, Display::kTableTop);
  M5.Display.drawString("Hum", Display::kColHum, Display::kTableTop);
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

void drawReadingRow(size_t row, const Reading& reading) {
  const int32_t y = Display::kRowsTop + static_cast<int32_t>(row) * Display::kRowHeight;
  M5.Display.fillRect(0, y, M5.Display.width(), Display::kRowHeight, TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(2);

  char timeText[24];
  char rawText[16];
  char percentText[16];
  char tempText[16];
  char humText[16];
  if (reading.timeOk) {
    formatTime(reading.datetime.time, timeText, sizeof(timeText));
  } else {
    snprintf(timeText, sizeof(timeText), "%lus", static_cast<unsigned long>(reading.elapsedSec));
  }
  snprintf(rawText, sizeof(rawText), "%u", reading.raw);
  snprintf(percentText, sizeof(percentText), "%.1f%%", reading.moisturePercent);
  if (reading.environmentOk) {
    snprintf(tempText, sizeof(tempText), "%.1fC", reading.temperatureC);
    snprintf(humText, sizeof(humText), "%.1f%%", reading.humidityPercent);
  } else {
    snprintf(tempText, sizeof(tempText), "ERR");
    snprintf(humText, sizeof(humText), "ERR");
  }

  M5.Display.drawString(timeText, Display::kColTime, y);
  M5.Display.drawString(rawText, Display::kColRaw, y);
  M5.Display.drawString(percentText, Display::kColPercent, y);
  M5.Display.drawString(tempText, Display::kColTemp, y);
  M5.Display.drawString(humText, Display::kColHum, y);
  M5.Display.drawLine(Display::kMarginX, y + 34, Display::kTableRight, y + 34, TFT_BLACK);
}

void readMoisture() {
  ++appState.sampleCount;
  Reading reading;
  reading.sample = appState.sampleCount;
  reading.elapsedSec = millis() / 1000;
  reading.timeOk = appState.ntpOk && M5.Rtc.getDateTime(&reading.datetime);
  reading.raw = static_cast<uint16_t>(analogRead(Sensor::kMoisturePin));
  reading.millivolts = analogReadMilliVolts(Sensor::kMoisturePin);
  reading.environmentOk = readSht30(reading.temperatureC, reading.humidityPercent);
  reading.minRaw = appState.latestReading.sample == 0 ? reading.raw : min(appState.latestReading.minRaw, reading.raw);
  reading.maxRaw = appState.latestReading.sample == 0 ? reading.raw : max(appState.latestReading.maxRaw, reading.raw);
  reading.adcPercent = rawToAdcPercent(reading.raw);
  reading.moisturePercent = rawToMoisturePercent(reading.raw);

  if (reading.environmentOk) {
    if (!appState.hasEnvironmentStats) {
      appState.minTemperatureC = reading.temperatureC;
      appState.maxTemperatureC = reading.temperatureC;
      appState.minHumidityPercent = reading.humidityPercent;
      appState.maxHumidityPercent = reading.humidityPercent;
      appState.hasEnvironmentStats = true;
    } else {
      appState.minTemperatureC = min(appState.minTemperatureC, reading.temperatureC);
      appState.maxTemperatureC = max(appState.maxTemperatureC, reading.temperatureC);
      appState.minHumidityPercent = min(appState.minHumidityPercent, reading.humidityPercent);
      appState.maxHumidityPercent = max(appState.maxHumidityPercent, reading.humidityPercent);
    }
  }

  appState.latestReading = reading;
  appState.hasPendingReading = true;
  appState.headerDirty = true;

  char timeText[24];
  if (reading.timeOk) {
    formatTime(reading.datetime.time, timeText, sizeof(timeText));
  } else {
    snprintf(timeText, sizeof(timeText), "%lus", static_cast<unsigned long>(reading.elapsedSec));
  }

  Serial.printf("[sample %lu] time=%s elapsed=%lus moisture_raw=%u moisture_mv=%lu moisture_percent=%.1f adc_percent=%.1f min_raw=%u max_raw=%u%s env=%s",
                static_cast<unsigned long>(reading.sample),
                timeText,
                static_cast<unsigned long>(reading.elapsedSec),
                reading.raw,
                static_cast<unsigned long>(reading.millivolts),
                reading.moisturePercent,
                reading.adcPercent,
                reading.minRaw,
                reading.maxRaw,
                reading.raw >= 4090 ? " saturated" : "",
                reading.environmentOk ? "OK" : "ERR");
  if (reading.environmentOk) {
    Serial.printf(" temp_c=%.2f humidity_percent=%.2f", reading.temperatureC, reading.humidityPercent);
  }
  Serial.printf(" pump=%s\n", appState.pumpRunning ? "ON" : "OFF");
}

void refreshDisplay() {
  if (M5.Display.displayBusy()) {
    return;
  }

  bool updated = false;
  bool wrapped = false;

  if (appState.hasPendingReading) {
    if (appState.nextDisplayRow >= Display::kVisibleRows) {
      clearRows();
      wrapped = true;
    }

    drawHeader();
    drawReadingRow(appState.nextDisplayRow, appState.latestReading);
    ++appState.nextDisplayRow;
    appState.hasPendingReading = false;
    updated = true;
  }

  if (appState.pumpUiDirty) {
    drawHeader();
    drawButtons();
    appState.pumpUiDirty = false;
    M5.Display.display(0, 0, M5.Display.width(), Display::kHeaderBottom);
    M5.Display.display(Display::kPumpButtonX - 3,
                       Display::kButtonY - 3,
                       Display::kWifiButtonX + Display::kButtonW - Display::kPumpButtonX + 6,
                       Display::kButtonH + 6);
  }

  if (!updated) {
    return;
  }

  if (wrapped) {
    M5.Display.display(0, 0, M5.Display.width(), Display::kButtonY);
    return;
  }

  const int32_t rowY = Display::kRowsTop + static_cast<int32_t>(appState.nextDisplayRow - 1) * Display::kRowHeight;
  M5.Display.display(0, 0, M5.Display.width(), Display::kHeaderBottom);
  M5.Display.display(0, rowY, M5.Display.width(), Display::kRowHeight);
}

void startPump() {
  if (appState.pumpRunning) {
    return;
  }

  digitalWrite(Sensor::kPumpPin, HIGH);
  appState.pumpRunning = true;
  appState.pumpStartedMs = millis();
  appState.pumpUiDirty = true;
  Serial.println("Pump started for 5 seconds");
}

void stopPump() {
  if (!appState.pumpRunning) {
    return;
  }

  digitalWrite(Sensor::kPumpPin, LOW);
  appState.pumpRunning = false;
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
  refreshHeaderAndButtons();
  readMoisture();
  refreshDisplay();

  const uint32_t now = millis();
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
    readMoisture();
  }

  if (now - appState.lastDisplayRefreshMs >= Display::kRefreshIntervalMs) {
    appState.lastDisplayRefreshMs = now;
    if (appState.pumpRunning) {
      appState.pumpUiDirty = true;
    }
    if (appState.headerDirty) {
      refreshHeaderAndButtons();
    }
    refreshDisplay();
  }

  M5.delay(10);
}
