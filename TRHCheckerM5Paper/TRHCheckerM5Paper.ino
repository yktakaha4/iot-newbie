#include <M5Unified.h>

namespace {
constexpr uint8_t kSht30Address = 0x44;
constexpr uint32_t kI2cFrequency = 400000;
constexpr uint32_t kSensorReadIntervalMs = 1000;
constexpr uint32_t kDisplayRefreshIntervalMs = 1000;
constexpr size_t kVisibleRows = 15;
constexpr int32_t kMarginX = 14;
constexpr int32_t kTitleY = 18;
constexpr int32_t kTableTop = 82;
constexpr int32_t kRowsTop = kTableTop + 44;
constexpr int32_t kRowHeight = 48;
constexpr int32_t kColDate = 14;
constexpr int32_t kColTime = 172;
constexpr int32_t kColTemp = 300;
constexpr int32_t kColHum = 410;
constexpr int32_t kTableRight = 526;
constexpr int32_t kStatusY = 924;

uint32_t lastSensorReadMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t sampleCount = 0;
size_t nextDisplayRow = 0;
bool hasPendingReading = false;

struct Reading {
  bool ok = false;
  m5::rtc_datetime_t datetime;
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
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

void formatDate(const m5::rtc_date_t& date, char* buffer, size_t length) {
  snprintf(buffer, length, "%04d/%02d/%02d", date.year, date.month, date.date);
}

void formatTime(const m5::rtc_time_t& time, char* buffer, size_t length) {
  snprintf(buffer, length, "%02d:%02d:%02d", time.hours, time.minutes, time.seconds);
}

void readSensor() {
  Reading reading;
  M5.Rtc.getDateTime(&reading.datetime);
  reading.ok = readSht30(reading.temperatureC, reading.humidityPercent);

  latestReading = reading;
  hasPendingReading = true;
  ++sampleCount;
}

void drawStaticFrame() {
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(top_left);

  M5.Display.setTextSize(3);
  M5.Display.drawString("TRH Checker", kMarginX, kTitleY);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Date", kColDate, kTableTop);
  M5.Display.drawString("Time", kColTime, kTableTop);
  M5.Display.drawString("Temp", kColTemp, kTableTop);
  M5.Display.drawString("Hum", kColHum, kTableTop);
  M5.Display.drawLine(kMarginX, kTableTop + 30, kTableRight, kTableTop + 30, TFT_BLACK);
}

void drawStatus() {
  char statusText[64];
  snprintf(statusText, sizeof(statusText), "Read %lus / Display %lus / Samples %lu",
           static_cast<unsigned long>(kSensorReadIntervalMs / 1000),
           static_cast<unsigned long>(kDisplayRefreshIntervalMs / 1000),
           static_cast<unsigned long>(sampleCount));
  M5.Display.fillRect(kMarginX, kStatusY, kTableRight - kMarginX, 28, TFT_WHITE);
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

  char dateText[16];
  char timeText[16];
  char tempText[16];
  char humText[16];
  formatDate(reading.datetime.date, dateText, sizeof(dateText));
  formatTime(reading.datetime.time, timeText, sizeof(timeText));
  if (reading.ok) {
    snprintf(tempText, sizeof(tempText), "%.1fC", reading.temperatureC);
    snprintf(humText, sizeof(humText), "%.1f%%", reading.humidityPercent);
  } else {
    snprintf(tempText, sizeof(tempText), "ERR");
    snprintf(humText, sizeof(humText), "ERR");
  }

  M5.Display.drawString(dateText, kColDate, y);
  M5.Display.drawString(timeText, kColTime, y);
  M5.Display.drawString(tempText, kColTemp, y);
  M5.Display.drawString(humText, kColHum, y);
  M5.Display.drawLine(kMarginX, y + 32, kTableRight, y + 32, TFT_BLACK);
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
  drawStatus();
  ++nextDisplayRow;
  hasPendingReading = false;

  if (wrapped) {
    M5.Display.display(0, kRowsTop, M5.Display.width(), M5.Display.height() - kRowsTop);
  } else {
    const int32_t rowY = kRowsTop + static_cast<int32_t>(nextDisplayRow - 1) * kRowHeight;
    M5.Display.display(0, rowY, M5.Display.width(), kRowHeight);
    M5.Display.display(kMarginX, kStatusY, kTableRight - kMarginX, 28);
  }
}
}  // namespace

void setup() {
  M5.begin();

  M5.Display.setEpdMode(epd_mode_t::epd_text);
  M5.Display.setRotation(0);

  M5.Display.clear(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  drawStaticFrame();
  drawStatus();
  M5.Display.display();
  readSensor();
  refreshDisplay();
  const uint32_t now = millis();
  lastSensorReadMs = now;
  lastDisplayRefreshMs = now;
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  if (now - lastSensorReadMs >= kSensorReadIntervalMs) {
    lastSensorReadMs = now;
    readSensor();
  }
  if (now - lastDisplayRefreshMs >= kDisplayRefreshIntervalMs) {
    lastDisplayRefreshMs = now;
    refreshDisplay();
  }
  M5.delay(10);
}
