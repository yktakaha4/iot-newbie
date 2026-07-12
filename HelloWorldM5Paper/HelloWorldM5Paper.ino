#include <M5Unified.h>

void setup() {
  M5.begin();

  M5.Display.setEpdMode(epd_mode_t::epd_text);
  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }

  M5.Display.clear(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(5);
  M5.Display.drawString("Hello World", M5.Display.width() / 2, M5.Display.height() / 2);
  M5.Display.display();
}

void loop() {
  M5.delay(1000);
}
