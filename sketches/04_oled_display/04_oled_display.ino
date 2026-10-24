// 04_oled_display: 1.3" OLED 128x64 SPI 표시 확인 (U8g2)
// 화면에 HOME 화면 예시(software-design.md 8-3절과 같은 5줄)와 프레임 카운터가 나오면 통과.
// 프레임 카운터는 마지막 줄 가운데(CPU와 48k 사이)에 표시합니다. 시리얼에 1프레임 전송 시간이 찍힙니다.
//
// 라이브러리: Arduino IDE Library Manager에서 "U8g2" 설치.
// SSD1306 모듈이면 아래 생성자를 U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI 로 바꾸세요.

#include <U8g2lib.h>
#include <SPI.h>

// 핀 배치 (docs/hardware-design.md 5장 초안): MOSI 11, SCK 13은 하드웨어 SPI 고정
#define PIN_OLED_CS   10
#define PIN_OLED_DC   24
#define PIN_OLED_RST  25

U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);

unsigned long frame = 0;

void setup() {
  Serial.begin(115200);
  oled.begin();
  Serial.println("04_oled_display 시작");
}

void loop() {
  unsigned long t0 = micros();

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, "[03] Plexi Crunch");
  oled.drawStr(110, 10, "BYP");
  oled.drawStr(0, 22, "plexi_nano.namb");
  oled.drawStr(0, 36, "AMP  Gain  Bass  Treb");
  oled.drawStr(0, 48, "     6.2   5.0   7.1");
  oled.drawStr(0, 62, "CPU 61%");
  char buf[16];
  snprintf(buf, sizeof(buf), "f%lu", frame++);
  oled.drawStr(54, 62, buf);  // 테스트용 프레임 카운터 (실제 HOME 화면에는 없음)
  oled.drawStr(104, 62, "48k");
  oled.sendBuffer();

  if (frame % 20 == 0) {
    Serial.printf("1프레임 전송 시간: %lu us\n", micros() - t0);
  }
  delay(50);  // 최대 20fps (software-design.md 8-3절)
}
