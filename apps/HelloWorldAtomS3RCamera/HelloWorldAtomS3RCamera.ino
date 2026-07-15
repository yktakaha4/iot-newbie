#include <M5Unified.h>

namespace {
constexpr uint32_t kLogIntervalMs = 1000;

uint32_t lastLogMs = 0;
uint32_t secondsSinceBoot = 0;

void drawScreen() {
  M5.Display.clear(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(top_left);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Hello", 8, 8);
  M5.Display.drawString("AtomS3R", 8, 34);
  M5.Display.drawString("Camera", 8, 60);

  M5.Display.setTextSize(1);

  char line[48];
  snprintf(line, sizeof(line), "Uptime: %lus", static_cast<unsigned long>(secondsSinceBoot));
  M5.Display.drawString(line, 8, 96);
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(500);

  M5.Display.setRotation(0);
  drawScreen();

  Serial.println("HelloWorldAtomS3RCamera started");
}

void loop() {
  M5.update();

  const uint32_t now = millis();
  if (now - lastLogMs >= kLogIntervalMs) {
    lastLogMs = now;
    secondsSinceBoot = now / 1000;
    drawScreen();
    Serial.printf("uptime=%lus\n", static_cast<unsigned long>(secondsSinceBoot));
  }

  M5.delay(10);
}
