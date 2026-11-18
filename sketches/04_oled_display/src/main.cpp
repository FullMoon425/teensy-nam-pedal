// 04_oled_display: Waveshare 2.23" OLED (SSD1305, 128x32) SPI 표시 확인 (U8g2) (H2)
// 저장소 루트에서:  pio run -d sketches/04_oled_display -t upload
// 시리얼 모니터:   pio device monitor -b 115200
//
// 목적: 화면 표시 확인 + 1프레임 전송 시간(sendBuffer) 기록.
//       나중에 오디오와 같이 돌릴 때 화면 전송이 오디오를 방해하는지 판단할 근거로 쓴다.
//       미터가 움직이므로 매 프레임(최대 20fps) 실제로 내용이 바뀐 화면을 보낸다.
//
// [통과 기준]
// - HOME 화면(프리셋 줄 + 노브 3개 라벨/값 + 오른쪽 IN/OUT 미터)이 깨지지 않고 표시됨
// - 노브 페이지가 PAGE_MS마다 EQ(Bass/Mid/Treb) ↔ IN/OUT(Gain/Gate/Mstr)으로 바뀌고, 미터 막대가 움직임
// - 시리얼에 찍히는 1프레임 전송 시간(평균/최대)과 실제 fps(20 근처)를 기록
// - 02 패스스루와 같이 연결했을 때 화면 갱신 중 '찌직' 잡음이 들리는지 확인해 기록
//
// CPU%, 샘플레이트 같은 상태 표시는 HOME에서 빼고 나중에 별도 화면(MENU > Info)에서 만든다.
// 이 스케치의 노브 값과 미터 레벨은 테스트용 가짜 값이다 (오디오 라이브러리 안 씀).
//
// 라이브러리: U8g2는 platformio.ini의 lib_deps로 첫 빌드 때 자동으로 받는다.
//             (Arduino IDE라면 Library Manager에서 U8g2 설치)

#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>

// ---- 컨트롤러 선택 ----
// Waveshare 2.23" OLED = SSD1305 128x32. U8g2 소스 주석상 Waveshare 모듈은 Adafruit 128x32와 같은 설정
// (x 오프셋 4)이라 ADAFRUIT 생성자를 쓴다. 화면이 좌우로 2픽셀 밀려 보이면 NONAME(오프셋 2)으로 바꿔 볼 것.
// 모듈 뒷면 저항 설정이 I2C로 되어 있으면 SPI로 바꿔야 한다 (Waveshare 기본값은 4선 SPI).

// ---- 핀 배치 (firmware/src/pins.h, docs/hardware-design.md 5장과 같게 유지) ----
// 하드웨어 SPI: MOSI 11, SCK 13 (고정, #define 불필요)
// 13번은 보드 LED(LED_BUILTIN)와 공용이라 화면을 갱신할 때 보드 LED가 깜빡인다. 정상 동작이다.
// 10번은 오디오실드의 SD CS와 공용이다. 그래서 실드 SD 슬롯은 쓰지 않는다 (SD는 Teensy 내장 슬롯 사용).
// 오디오실드 사용 핀(I2S 7, 8, 20, 21, 23 / I2C 18, 19)은 건드리지 않는다.
#define PIN_OLED_MOSI  11  // 표시용 (하드웨어 고정)
#define PIN_OLED_SCK   13  // 표시용 (하드웨어 고정)
#define PIN_OLED_CS    10
#define PIN_OLED_DC    24
#define PIN_OLED_RST   25

U8G2_SSD1305_128X32_ADAFRUIT_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
// U8G2_SSD1305_128X32_NONAME_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
#define OLED_NAME "SSD1305 (Waveshare 2.23\")"

// ---- 타이밍 ----
#define FRAME_MS   50    // 최대 20fps (software-design.md 8-3절)
#define REPORT_MS  1000  // 시리얼 통계 출력 주기
#define PAGE_MS    2000  // 테스트용 노브 페이지 자동 전환 주기 (실제로는 버튼으로 전환). 0이면 전환 안 함

// ---- HOME 화면 레이아웃 (나중에 실제 UI 코드로 옮길 부분) ----
// 3 = 6x10 글꼴, 프리셋 / 노브 라벨 / 노브 값 (모델 파일명 생략, 글자가 커서 페달 위에서 읽기 쉬움)
// 4 = 5x7 글꼴, 프리셋 / 모델 파일명 / 노브 라벨 / 노브 값
#define HOME_LINES  3

#define SCREEN_W  128
#define SCREEN_H  32
#define KNOB_COUNT 3  // 물리 포트 3개 = 한 페이지에 노브 3개

#if HOME_LINES == 4
#define UI_FONT  u8g2_font_5x7_tf  // 글자 5x7, 한 줄 최대 25자
// 기준선 y, 8px 간격. 5x7은 위로 6픽셀, 아래로 1픽셀 → 첫 줄 위 0, 마지막 줄 바닥 31
const uint8_t LINE_Y[4] = {6, 14, 22, 30};
#define LINE_PRESET  0
#define LINE_MODEL   1
#define LINE_LABEL   2
#define LINE_VALUE   3
#define METER_H      6  // 미터 막대 높이 = 글자 높이
#else
#define UI_FONT  u8g2_font_6x10_tf  // 글자 6x10, 한 줄 최대 21자
// 기준선 y. 6x10은 위로 7픽셀, 아래로 2픽셀 → 첫 줄 위 0, 마지막 줄 바닥 31
const uint8_t LINE_Y[3] = {7, 18, 29};
#define LINE_PRESET  0
#define LINE_LABEL   1
#define LINE_VALUE   2
#define METER_H      7
#endif

// 노브 영역(왼쪽)과 미터 영역(오른쪽, 예전 Master 자리)
#define KNOB_AREA_W  90                 // 0~89: 노브 3열, 열마다 30px (6x10 기준 5자까지)
#define METER_X      92                 // 미터 영역 시작 x ("I"/"O" 글자 포함)
#define METER_BAR_X  (METER_X + 7)      // 막대 시작 x (글자 6px + 1px 간격)
#define METER_BAR_W  (SCREEN_W - METER_BAR_X)  // 막대 폭 (테두리 포함)

// 프리셋 번호+이름(왼쪽), 바이패스 표시(오른쪽 끝)
const char* const HOME_PRESET = "[03] Plexi Crunch";
const char* const HOME_BYPASS = "BYP";
// 모델 파일명 (4줄 배치에서만 표시)
const char* const HOME_MODEL = "plexi_nano.namb";

// 노브 페이지: 물리 포트 3개를 두 페이지로 전환해서 6개 파라미터를 다룬다.
// 열 위치가 페이지와 상관없이 같아서, 페이지를 바꿔도 같은 포트는 같은 자리에 보인다.
struct KnobPage {
  const char* name;
  const char* labels[KNOB_COUNT];
  const char* values[KNOB_COUNT];  // 테스트용 고정값
};
const KnobPage PAGES[] = {
  {"EQ",     {"Bass", "Mid",  "Treb"}, {"5.0", "4.5", "7.1"}},
  {"IN/OUT", {"Gain", "Gate", "Mstr"}, {"6.2", "3.0", "8.0"}},  // 입력 게인 / 노이즈게이트 / 마스터
};
const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

// ---- 상태 ----
int page = 0;
unsigned long lastPageAt = 0;
unsigned long lastFrameAt = 0;  // 마지막 프레임 시작 시각 (ms)
unsigned long lastReportAt = 0;
uint32_t sendSumUs = 0;   // 이번 1초 동안의 sendBuffer 시간 합
uint32_t sendMaxUs = 0;   // 이번 1초 동안의 최대값 (출력 후 초기화)
uint32_t sendFrames = 0;  // 이번 1초 동안 보낸 프레임 수

// 오른쪽 끝 정렬 x좌표. 문자열이 화면보다 넓으면 0 (화면 밖으로 안 나가게)
int rightX(const char* s) {
  int w = oled.getStrWidth(s);
  return w < SCREEN_W ? SCREEN_W - w : 0;
}

// 열 가운데에 문자열 그리기 (열보다 넓으면 열 왼쪽에 맞춤)
void drawCentered(int colX, int colW, int y, const char* s) {
  int x = colX + (colW - oled.getStrWidth(s)) / 2;
  oled.drawStr(x < colX ? colX : x, y, s);
}

// 가로 레벨 미터. level 0.0~1.0. 테두리 안을 level만큼 채운다.
// 실제 펌웨어에서는 dBFS를 0~1로 바꿔 넣는다 (예: -48dB → 0, 0dB → 1).
void drawMeter(int x, int baselineY, int w, int h, float level) {
  if (level < 0) level = 0;
  if (level > 1) level = 1;
  int top = baselineY - h + 1;  // 글자와 같은 높이에 맞춤
  oled.drawFrame(x, top, w, h);
  int fill = (int)((w - 2) * level + 0.5f);
  if (fill > 0) oled.drawBox(x + 1, top + 1, fill, h - 2);
}

// 테스트용 가짜 레벨: 서로 다른 속도로 오르내림
float fakeLevel(unsigned long ms, float hz, float phase) {
  return 0.5f + 0.45f * sinf(2.0f * PI * hz * ms / 1000.0f + phase);
}

void drawHome(unsigned long now) {
  oled.setFont(UI_FONT);

  // 프리셋 줄
  oled.drawStr(0, LINE_Y[LINE_PRESET], HOME_PRESET);
  oled.drawStr(rightX(HOME_BYPASS), LINE_Y[LINE_PRESET], HOME_BYPASS);

#if HOME_LINES == 4
  oled.drawStr(0, LINE_Y[LINE_MODEL], HOME_MODEL);
#endif

  // 노브 3열: 라벨과 값을 같은 열 가운데에 맞춤
  const KnobPage& p = PAGES[page];
  const int colW = KNOB_AREA_W / KNOB_COUNT;
  for (int i = 0; i < KNOB_COUNT; i++) {
    drawCentered(i * colW, colW, LINE_Y[LINE_LABEL], p.labels[i]);
    drawCentered(i * colW, colW, LINE_Y[LINE_VALUE], p.values[i]);
  }

  // IN/OUT 미터 (라벨 줄 = 입력, 값 줄 = 출력)
  oled.drawStr(METER_X, LINE_Y[LINE_LABEL], "I");
  oled.drawStr(METER_X, LINE_Y[LINE_VALUE], "O");
  drawMeter(METER_BAR_X, LINE_Y[LINE_LABEL], METER_BAR_W, METER_H, fakeLevel(now, 0.7f, 0.0f));
  drawMeter(METER_BAR_X, LINE_Y[LINE_VALUE], METER_BAR_W, METER_H, fakeLevel(now, 0.45f, 1.0f));
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}  // 시리얼 모니터 연결을 최대 2초 기다림

  oled.begin();

  Serial.println("04_oled_display 시작");
  Serial.printf("컨트롤러: %s (%dx%d, 4선 하드웨어 SPI)\n", OLED_NAME, SCREEN_W, SCREEN_H);
  Serial.println("핀 배치:");
  Serial.printf("  MOSI %d, SCK %d (하드웨어 SPI 고정, SCK는 보드 LED와 공용)\n", PIN_OLED_MOSI, PIN_OLED_SCK);
  Serial.printf("  CS %d (실드 SD CS와 공용, 실드 SD 슬롯 사용 안 함), DC %d, RST %d\n",
                PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
  Serial.println("  VCC 3.3V, GND");
  Serial.printf("HOME 화면 %d줄 배치, 노브 페이지 %d개 (%dms마다 자동 전환)\n", HOME_LINES, PAGE_COUNT, PAGE_MS);
  Serial.printf("최대 %d fps, %dms마다 전송 시간 출력\n", 1000 / FRAME_MS, REPORT_MS);

  lastReportAt = lastPageAt = millis();
}

void loop() {
  unsigned long now = millis();

  // 테스트용 페이지 자동 전환
  if (PAGE_MS > 0 && now - lastPageAt >= PAGE_MS) {
    lastPageAt = now;
    page = (page + 1) % PAGE_COUNT;
    Serial.printf("노브 페이지: %s\n", PAGES[page].name);
  }

  // 프레임: FRAME_MS가 지났을 때만 그린다 (늦어졌을 때 몰아서 그리지 않도록 now로 맞춤)
  if (now - lastFrameAt >= FRAME_MS) {
    lastFrameAt = now;

    oled.clearBuffer();
    drawHome(now);

    uint32_t t0 = micros();
    oled.sendBuffer();  // 측정 구간: SPI 전송만
    uint32_t dt = micros() - t0;

    sendSumUs += dt;
    if (dt > sendMaxUs) sendMaxUs = dt;
    sendFrames++;
  }

  // 1초마다 통계 출력
  if (now - lastReportAt >= REPORT_MS) {
    unsigned long elapsed = now - lastReportAt;
    lastReportAt = now;
    if (sendFrames > 0) {
      Serial.printf("전송 평균 %lu us / 최대 %lu us, 실제 %.1f fps\n",
                    (unsigned long)(sendSumUs / sendFrames), (unsigned long)sendMaxUs,
                    sendFrames * 1000.0f / elapsed);
    } else {
      Serial.println("이번 1초 동안 보낸 프레임 없음");
    }
    sendSumUs = 0;
    sendMaxUs = 0;
    sendFrames = 0;
  }
}
