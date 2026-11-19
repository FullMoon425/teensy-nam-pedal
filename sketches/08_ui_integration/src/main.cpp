// 08_ui_integration: 조작계(03) + OLED(04) 통합 테스트 (H2)
// 저장소 루트에서:  pio run -d sketches/08_ui_integration -t upload
// 시리얼 모니터:   pio device monitor -b 115200
//
// 03(포트·인코더·버튼)과 04(화면)를 각각 통과한 뒤에 돌린다. 문제가 생기면 03/04 단독으로 돌려 원인을 가른다.
// software-design.md 8-1~8-3절의 HOME 동작을 오디오 없이 흉내 낸다:
//   포트 3개      → 현재 노브 페이지의 파라미터 3개 (캐치 방식, 1.5초 큰 글씨 팝업)
//   인코더 회전   → 프리셋 미리보기 (짧게 누름 = 로드, 3초 무조작 = 취소)
//   인코더 짧게   → 노브 페이지 전환 EQ ↔ IN/OUT (미리보기 중에는 로드)
//   인코더 길게   → MENU (아직 없음, 시리얼에만 출력)
//   풋스위치 짧게 → 바이패스 토글 (LED 켜짐 = 이펙트 켜짐, 화면에 BYP)
//   풋스위치 길게 → 다음 프리셋 (마지막 다음은 첫 프리셋)
//
// [통과 기준]
// - 포트를 돌리면 화면 값과 팝업이 바로 따라오고, 손을 떼면 값이 안 떨림 (03 단독일 때와 같아야 함)
// - 페이지/프리셋을 바꾼 직후에는 포트가 잠기고(값 괄호 표시), 노브가 저장값을 지나가면 풀림
// - 인코더 한 칸에 프리셋 1칸, 미리보기는 반전 표시, 짧게 누름으로 로드, 3초 지나면 취소
// - 풋스위치 짧게 = LED와 BYP 표시 토글, 길게 = 600ms에 바로 다음 프리셋
// - 시리얼 1초 통계: 전송 평균/최대 us, 실제 fps(20 근처), 루프 최대 us 기록
//   (루프 최대 = 입력을 못 보는 최악 시간. 디바운스 20ms보다 충분히 작아야 함)
//
// 노브 값과 미터 레벨은 테스트용 가짜 값이다 (오디오 라이브러리 안 씀).

#include <Arduino.h>
#include <Encoder.h>
#include <U8g2lib.h>
#include <SPI.h>

// ================= 핀 배치 (firmware/src/pins.h와 같게 유지) =================
// 13번은 OLED SCK + 보드 LED 공용 (화면 갱신 때 보드 LED가 깜빡이는 것은 정상).
// 10번은 오디오실드 SD CS와 공용이라 실드 SD 슬롯은 쓰지 않는다.
#define PIN_POT1      14  // A0
#define PIN_POT2      15  // A1
#define PIN_POT3      16  // A2
#define PIN_ENC_A     2
#define PIN_ENC_B     3
#define PIN_ENC_SW    4
#define PIN_FOOTSW    5
#define PIN_LED       9
#define PIN_OLED_MOSI 11  // 하드웨어 SPI 고정
#define PIN_OLED_SCK  13  // 하드웨어 SPI 고정
#define PIN_OLED_CS   10
#define PIN_OLED_DC   24
#define PIN_OLED_RST  25

// ================= 동작 상수 =================
#define DEBOUNCE_MS      20
#define LONG_PRESS_MS    600
#define POT_HYST         0.01f  // 1%
#define POT_INTERVAL_MS  10
#define ENC_COUNTS_PER_DETENT 4  // 한 칸에 2씩 바뀌는 부품이면 2
#define CATCH_WINDOW     0.02f  // 노브가 저장값 ±2% 안에 들어오면 잠금 해제
#define POPUP_MS         1500   // 노브 조작 팝업 표시 시간
#define PREVIEW_MS       3000   // 프리셋 미리보기 무조작 취소 시간
#define FRAME_MS         50     // 최대 20fps
#define REPORT_MS        1000

// ================= 화면 =================
U8G2_SSD1305_128X32_ADAFRUIT_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
// U8G2_SSD1305_128X32_NONAME_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);

#define SCREEN_W     128
#define UI_FONT      u8g2_font_6x10_tf   // HOME 3줄
#define POPUP_FONT   u8g2_font_10x20_tf  // 팝업 값
const uint8_t LINE_Y[3] = {7, 18, 29};   // 기준선 (6x10: 위 7px, 아래 2px)
#define LINE_PRESET  0
#define LINE_LABEL   1
#define LINE_VALUE   2
#define KNOB_AREA_W  90                  // 노브 3열, 열마다 30px
#define METER_X      92
#define METER_BAR_X  (METER_X + 7)
#define METER_BAR_W  (SCREEN_W - METER_BAR_X)
#define METER_H      7

// ================= 파라미터·페이지·프리셋 =================
enum ParamId { P_BASS, P_MID, P_TREBLE, P_GAIN, P_GATE, P_MASTER, P_COUNT };
const char* const PARAM_NAME[P_COUNT]  = {"Bass", "Mid", "Treble", "Gain", "Gate", "Master"};  // 팝업용
const char* const PARAM_SHORT[P_COUNT] = {"Bass", "Mid", "Treb", "Gain", "Gate", "Mstr"};      // HOME 라벨 (5자 이내)

struct KnobPage {
  const char* name;
  ParamId pot[3];  // 포트 1/2/3이 맡는 파라미터
};
const KnobPage PAGES[] = {
  {"EQ",     {P_BASS, P_MID, P_TREBLE}},
  {"IN/OUT", {P_GAIN, P_GATE, P_MASTER}},
};
const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

struct Preset {
  const char* name;
  float p[P_COUNT];  // 0~1, ParamId 순서
};
const Preset PRESETS[] = {  // 테스트용 가짜 프리셋
  {"Plexi Crunch", {0.50f, 0.45f, 0.71f, 0.62f, 0.30f, 0.80f}},
  {"Clean Twin",   {0.40f, 0.55f, 0.60f, 0.25f, 0.20f, 0.70f}},
  {"Lead 5150",    {0.60f, 0.35f, 0.65f, 0.85f, 0.45f, 0.75f}},
  {"Fuzz Box",     {0.70f, 0.50f, 0.40f, 0.95f, 0.10f, 0.60f}},
};
const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// ================= 상태 =================
float params[P_COUNT];
int presetIdx = 0;
int page = 0;
bool bypass = false;

bool previewing = false;
int previewIdx = 0;
unsigned long previewAt = 0;  // 마지막 인코더 조작 시각

int popupParam = -1;
unsigned long popupAt = 0;

// 포트
const uint8_t potPins[3] = {PIN_POT1, PIN_POT2, PIN_POT3};
float potPos[3] = {-1, -1, -1};  // 히스테리시스 적용된 노브 위치 0~1
bool potLocked[3] = {true, true, true};
unsigned long potLastReadAt = 0;

// 인코더
Encoder enc(PIN_ENC_A, PIN_ENC_B);
long encBase = 0;

// 버튼 (03과 같은 구조)
enum ButtonEvent { BTN_NONE, BTN_SHORT, BTN_LONG };
struct Button {
  uint8_t pin;
  const char* name;
  bool raw;
  unsigned long rawSince;
  bool pressed;
  unsigned long pressedAt;
  bool longFired;
};
Button encBtn  = {PIN_ENC_SW, "인코더 버튼"};
Button footBtn = {PIN_FOOTSW, "풋스위치"};

// 통계
unsigned long lastFrameAt = 0, lastReportAt = 0;
uint32_t sendSumUs = 0, sendMaxUs = 0, sendFrames = 0, loopMaxUs = 0;

// ================= 입력 =================
void buttonBegin(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.raw = digitalRead(b.pin) == LOW;
  b.pressed = b.raw;
  b.rawSince = b.pressedAt = millis();
  b.longFired = b.pressed;  // 켤 때 이미 눌려 있으면 이벤트 안 냄
}

// 짧게: 뗄 때 / 길게: 누른 지 LONG_PRESS_MS 되는 순간. 길게가 났으면 뗄 때 짧게 없음.
ButtonEvent buttonPoll(Button& b, unsigned long now) {
  bool r = digitalRead(b.pin) == LOW;
  if (r != b.raw) {
    b.raw = r;
    b.rawSince = now;
  }
  if (b.raw != b.pressed && now - b.rawSince >= DEBOUNCE_MS) {
    b.pressed = b.raw;
    if (b.pressed) {
      b.pressedAt = b.rawSince;
      b.longFired = false;
    } else if (!b.longFired) {
      return BTN_SHORT;
    }
  }
  if (b.pressed && !b.longFired && now - b.pressedAt >= LONG_PRESS_MS) {
    b.longFired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

// 디텐트 단위 회전량 (+ 시계, - 반시계)
int encoderSteps() {
  long raw = enc.read();
  int steps = 0;
  while (raw - encBase >= ENC_COUNTS_PER_DETENT) { encBase += ENC_COUNTS_PER_DETENT; steps++; }
  while (encBase - raw >= ENC_COUNTS_PER_DETENT) { encBase -= ENC_COUNTS_PER_DETENT; steps--; }
  return steps;
}

// 페이지·프리셋이 바뀌면 포트 3개를 모두 잠근다 (캐치)
void lockPots(const char* why) {
  for (int i = 0; i < 3; i++) potLocked[i] = true;
  Serial.printf("포트 잠금 (%s)\n", why);
}

void loadPreset(int idx) {
  presetIdx = idx;
  for (int i = 0; i < P_COUNT; i++) params[i] = PRESETS[idx].p[i];
  Serial.printf("프리셋 로드: [%02d] %s\n", idx + 1, PRESETS[idx].name);
  lockPots("프리셋 변경");
}

void pollPots(unsigned long now) {
  if (now - potLastReadAt < POT_INTERVAL_MS) return;
  potLastReadAt = now;

  for (int i = 0; i < 3; i++) {
    float pos = analogRead(potPins[i]) / 4095.0f;
    float prev = potPos[i];
    if (prev >= 0 && fabsf(pos - prev) < POT_HYST) continue;  // 1% 안쪽 변화는 무시
    potPos[i] = pos;
    if (prev < 0) continue;  // 첫 읽기는 위치만 기억

    ParamId id = PAGES[page].pot[i];
    if (potLocked[i]) {
      // 저장값 근처에 오거나 저장값을 가로지르면 잠금 해제
      float v = params[id];
      bool crossed = (prev - v) * (pos - v) <= 0;
      if (fabsf(pos - v) <= CATCH_WINDOW || crossed) {
        potLocked[i] = false;
        Serial.printf("POT%d 잠금 해제 (%s)\n", i + 1, PARAM_NAME[id]);
      } else {
        Serial.printf("POT%d 잠김: 노브 %.1f / 저장 %.1f\n", i + 1, pos * 10, v * 10);
        continue;
      }
    }
    params[id] = pos;
    popupParam = id;
    popupAt = now;
    Serial.printf("POT%d %s %.1f\n", i + 1, PARAM_NAME[id], pos * 10);
  }
}

void handleInputs(unsigned long now) {
  pollPots(now);

  // 인코더 회전 → 프리셋 미리보기
  int steps = encoderSteps();
  if (steps) {
    if (!previewing) {
      previewing = true;
      previewIdx = presetIdx;
    }
    previewIdx = ((previewIdx + steps) % PRESET_COUNT + PRESET_COUNT) % PRESET_COUNT;
    previewAt = now;
    popupParam = -1;  // 미리보기 중에는 팝업 끔
    Serial.printf("미리보기: [%02d] %s\n", previewIdx + 1, PRESETS[previewIdx].name);
  }
  if (previewing && now - previewAt >= PREVIEW_MS) {
    previewing = false;
    Serial.println("미리보기 취소 (3초 무조작)");
  }

  // 인코더 버튼
  ButtonEvent e = buttonPoll(encBtn, now);
  if (e == BTN_SHORT) {
    if (previewing) {
      previewing = false;
      loadPreset(previewIdx);
    } else {
      page = (page + 1) % PAGE_COUNT;
      Serial.printf("노브 페이지: %s\n", PAGES[page].name);
      lockPots("페이지 변경");
    }
  } else if (e == BTN_LONG) {
    Serial.println("인코더 버튼 길게 → MENU (이 스케치에는 없음)");
  }

  // 풋스위치
  e = buttonPoll(footBtn, now);
  if (e == BTN_SHORT) {
    bypass = !bypass;
    digitalWrite(PIN_LED, bypass ? LOW : HIGH);
    Serial.printf("풋스위치 짧게 → %s (LED %s)\n", bypass ? "바이패스" : "이펙트 켜짐", bypass ? "OFF" : "ON");
  } else if (e == BTN_LONG) {
    previewing = false;
    Serial.println("풋스위치 길게 → 다음 프리셋");
    loadPreset((presetIdx + 1) % PRESET_COUNT);
  }
}

// ================= 그리기 =================
int rightX(const char* s) {
  int w = oled.getStrWidth(s);
  return w < SCREEN_W ? SCREEN_W - w : 0;
}

void drawCentered(int colX, int colW, int y, const char* s) {
  int x = colX + (colW - oled.getStrWidth(s)) / 2;
  oled.drawStr(x < colX ? colX : x, y, s);
}

void drawMeter(int x, int baselineY, int w, int h, float level) {
  if (level < 0) level = 0;
  if (level > 1) level = 1;
  int top = baselineY - h + 1;
  oled.drawFrame(x, top, w, h);
  int fill = (int)((w - 2) * level + 0.5f);
  if (fill > 0) oled.drawBox(x + 1, top + 1, fill, h - 2);
}

float fakeLevel(unsigned long ms, float hz, float phase) {
  return 0.5f + 0.45f * sinf(2.0f * PI * hz * ms / 1000.0f + phase);
}

// 0~1 값을 0.0~10.0 문자열로. 10은 "10"으로 줄여 괄호를 붙여도 30px 칸(5자)에 들어가게
void fmtValue(char* buf, size_t n, float v, bool locked) {
  char num[6];
  if (v * 10 >= 9.95f) snprintf(num, sizeof(num), "10");
  else snprintf(num, sizeof(num), "%.1f", v * 10);
  snprintf(buf, n, locked ? "(%s)" : "%s", num);
}

// 1줄: 프리셋 (미리보기 중이면 반전), 오른쪽 끝 BYP
void drawPresetLine() {
  bool inv = previewing;
  int idx = inv ? previewIdx : presetIdx;
  char buf[24];
  snprintf(buf, sizeof(buf), "[%02d] %s", idx + 1, PRESETS[idx].name);
  if (inv) {
    oled.drawBox(0, 0, SCREEN_W, 9);
    oled.setDrawColor(0);
  }
  oled.drawStr(0, LINE_Y[LINE_PRESET], buf);
  if (bypass) oled.drawStr(rightX("BYP"), LINE_Y[LINE_PRESET], "BYP");
  oled.setDrawColor(1);
}

void drawHome(unsigned long now) {
  oled.setFont(UI_FONT);
  drawPresetLine();

  // 노브 3열. 잠긴 포트는 저장값을 괄호로 표시
  const int colW = KNOB_AREA_W / 3;
  char buf[8];
  for (int i = 0; i < 3; i++) {
    ParamId id = PAGES[page].pot[i];
    drawCentered(i * colW, colW, LINE_Y[LINE_LABEL], PARAM_SHORT[id]);
    fmtValue(buf, sizeof(buf), params[id], potLocked[i]);
    drawCentered(i * colW, colW, LINE_Y[LINE_VALUE], buf);
  }

  // IN/OUT 미터 (바이패스면 출력 = 입력)
  float in = fakeLevel(now, 0.7f, 0.0f);
  float out = bypass ? in : fakeLevel(now, 0.45f, 1.0f) * params[P_MASTER] * 1.25f;
  oled.drawStr(METER_X, LINE_Y[LINE_LABEL], "I");
  oled.drawStr(METER_X, LINE_Y[LINE_VALUE], "O");
  drawMeter(METER_BAR_X, LINE_Y[LINE_LABEL], METER_BAR_W, METER_H, in);
  drawMeter(METER_BAR_X, LINE_Y[LINE_VALUE], METER_BAR_W, METER_H, out);
}

// 노브 조작 팝업: 파라미터 이름 + 큰 값 + 아래 가로 막대
void drawPopup() {
  float v = params[popupParam];
  char buf[8];
  fmtValue(buf, sizeof(buf), v, false);
  oled.setFont(UI_FONT);
  oled.drawStr(0, 9, PARAM_NAME[popupParam]);
  oled.setFont(POPUP_FONT);
  oled.drawStr(rightX(buf), 22, buf);
  oled.drawFrame(0, 26, SCREEN_W, 6);
  int fill = (int)((SCREEN_W - 2) * v + 0.5f);
  if (fill > 0) oled.drawBox(1, 27, fill, 4);
}

void drawScreen(unsigned long now) {
  oled.clearBuffer();
  oled.setFontMode(1);  // 투명 글자 (반전 줄에서 검은 글자를 그리기 위해)
  if (popupParam >= 0 && now - popupAt < POPUP_MS) {
    drawPopup();
  } else {
    popupParam = -1;
    drawHome(now);
  }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  analogReadResolution(12);
  analogReadAveraging(16);
  buttonBegin(encBtn);
  buttonBegin(footBtn);
  pinMode(PIN_LED, OUTPUT);
  oled.begin();

  Serial.println("08_ui_integration 시작");
  Serial.println("핀 배치:");
  Serial.printf("  포트 %d/%d/%d, 인코더 A/B/SW %d/%d/%d, 풋스위치 %d, LED %d\n",
                PIN_POT1, PIN_POT2, PIN_POT3, PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, PIN_FOOTSW, PIN_LED);
  Serial.printf("  OLED SSD1305 128x32: MOSI %d, SCK %d, CS %d, DC %d, RST %d\n",
                PIN_OLED_MOSI, PIN_OLED_SCK, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
  Serial.printf("설정: 디바운스 %dms, 길게 %dms, 포트 히스테리시스 %.0f%%, 캐치 ±%.0f%%, 최대 %d fps\n",
                DEBOUNCE_MS, LONG_PRESS_MS, POT_HYST * 100, CATCH_WINDOW * 100, 1000 / FRAME_MS);

  loadPreset(0);
  digitalWrite(PIN_LED, HIGH);  // 이펙트 켜짐
  lastReportAt = millis();
}

void loop() {
  uint32_t loopStart = micros();
  unsigned long now = millis();

  handleInputs(now);

  if (now - lastFrameAt >= FRAME_MS) {
    lastFrameAt = now;
    drawScreen(now);
    uint32_t t0 = micros();
    oled.sendBuffer();  // 측정 구간: SPI 전송만
    uint32_t dt = micros() - t0;
    sendSumUs += dt;
    if (dt > sendMaxUs) sendMaxUs = dt;
    sendFrames++;
  }

  if (now - lastReportAt >= REPORT_MS) {
    unsigned long elapsed = now - lastReportAt;
    lastReportAt = now;
    if (sendFrames > 0) {
      Serial.printf("[통계] 전송 평균 %lu us / 최대 %lu us, %.1f fps, 루프 최대 %lu us\n",
                    (unsigned long)(sendSumUs / sendFrames), (unsigned long)sendMaxUs,
                    sendFrames * 1000.0f / elapsed, (unsigned long)loopMaxUs);
    }
    sendSumUs = sendMaxUs = sendFrames = loopMaxUs = 0;
  }

  uint32_t lt = micros() - loopStart;
  if (lt > loopMaxUs) loopMaxUs = lt;
}
