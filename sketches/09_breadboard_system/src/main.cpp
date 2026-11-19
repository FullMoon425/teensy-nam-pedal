// 09_breadboard_system: 페달 전체를 브레드보드에서 (NAM 없음) (H2)
// 저장소 루트에서:  pio run -d sketches/09_breadboard_system -t upload
// 시리얼 모니터:   pio device monitor -b 115200
//
// 08(조작계 + 화면)에 실제 오디오를 붙인다. 노브가 소리를 바꾸고, 미터는 실제 피크를 보여 준다.
//   기타 → 입력 버퍼(9V) → 실드 LINE IN
//     → [Peak IN] → Gain + 소프트 클리퍼(tanh, NAM 자리) → EQ 3밴드 → Master → 바이패스 페이드 → [Peak OUT]
//     → LINE OUT / 헤드폰
// Gate 노브는 화면에만 있고 처리는 아직 없다 (다음 단계에서 dsp/Gate로).
//
// [통과 기준]
// 1. "Flat Test" 프리셋(EQ 0, Gain/Master 0dB)에서 바이패스와 이펙트의 음량이 같고(약하게 칠 때), 전환 때 딸깍 없음
// 2. 노브를 돌리면 소리가 바로 바뀌고, 돌리는 동안 지퍼 잡음(지직지직) 없음
// 3. 기타 볼륨 0, Master 높게 두고 시리얼 'o'로 화면 켜고/끄기 → 잡음 차이 없음
// 4. 오디오가 도는 중에도 포트가 08과 같이 안정적임
// 5. 세게 쳐도 IN 미터에 CLIP이 안 뜨는 lineInLevel을 'l'/'L'로 찾아 hardware-design.md 3-1절에 기록
// 6. [통계]의 CPU%, 오디오 메모리 최대, 루프 최대 us 기록 (NAM이 쓸 수 있는 여유의 기준선)
//
// 시리얼 명령: o 화면 켜기/끄기 | f 프레임 20/5/1fps | b 바이패스 | l/L lineInLevel 내림/올림 | s 통계 출력 켜기/끄기
//             p 포트 읽기 켜기/끄기 | n 다음 프리셋 (인코더·풋스위치 없이 시험할 때)
//             c 화면 밝기 255/128/32/1 (OLED 잡음이 패널 전원 때문인지 SPI 전송 때문인지 가르기)

#include <Arduino.h>
#include <Audio.h>
#include <Encoder.h>
#include <U8g2lib.h>
#include <SPI.h>

// ================= 핀 배치 (firmware/src/pins.h와 같게 유지) =================
// 오디오실드: I2S 7, 8, 20, 21, 23 / I2C 18, 19 (건드리지 않음)
// 13번은 OLED SCK + 보드 LED 공용. 10번은 실드 SD CS와 공용이라 실드 SD 슬롯은 쓰지 않음.
#define PIN_POT1      14
#define PIN_POT2      15
#define PIN_POT3      16
#define PIN_ENC_A     2
#define PIN_ENC_B     3
#define PIN_ENC_SW    4
#define PIN_FOOTSW    5
#define PIN_LED       9
#define PIN_OLED_CS   10
#define PIN_OLED_DC   24
#define PIN_OLED_RST  25

// ================= 동작 상수 =================
// 포트를 아직 안 달았으면 0. 아날로그 핀이 떠 있으면 값이 제멋대로 움직여 Gain/Master가 튐(갑자기 큰 소리).
// 실행 중에는 시리얼 'p'로도 켜고 끌 수 있음
#define USE_POTS         0

#define DEBOUNCE_MS      20
#define LONG_PRESS_MS    600
#define POT_HYST         0.01f
#define POT_INTERVAL_MS  10
#define ENC_COUNTS_PER_DETENT 4
#define CATCH_WINDOW     0.02f
#define POPUP_MS         1500
#define PREVIEW_MS       3000
#define REPORT_MS        1000
#define BYPASS_FADE_MS   8       // 바이패스 크로스페이드 (software-design.md 6장: 5~10ms)

// 파라미터 범위 (software-design.md 6장)
#define GAIN_MIN_DB    -12.0f
#define GAIN_MAX_DB     24.0f
#define MASTER_MIN_DB  -40.0f
#define MASTER_MAX_DB    6.0f
#define EQ_RANGE_DB     12.0f    // ±12dB, 노브 0.5 = 0dB
#define EQ_BASS_HZ     120.0f
#define EQ_MID_HZ      700.0f
#define EQ_MID_Q         0.7f
#define EQ_TREBLE_HZ  3000.0f
#define CLIP_CEIL        0.7f    // 소프트 클리퍼 최대 출력(약 -3dBFS). EQ 부스트 여유

// 미터
#define METER_FLOOR_DB  -48.0f
#define CLIP_DB          -0.5f   // 이 이상이면 CLIP
#define CLIP_SHOW_MS    1000
#define PEAK_HOLD_MS    1000

// 코덱
#define LINE_OUT_LEVEL  29
#define HEADPHONE_VOL   0.5f
const float LINE_IN_VPP[16] = {3.12f, 2.63f, 2.22f, 1.87f, 1.58f, 1.33f, 1.11f, 0.94f,
                               0.79f, 0.67f, 0.56f, 0.48f, 0.40f, 0.34f, 0.29f, 0.24f};

// ================= 오디오 =================
// SGTL5000 enable()은 44.1kHz(0x0004)를 쓰므로 48kHz(0x0008)로 덮어씀 (06과 같음)
class Codec48k : public AudioControlSGTL5000 {
public:
  bool setSysFs48k() { return write(0x0004, 0x0008); }
};

// 게인(블록마다 선형 램프로 지퍼 잡음 방지) + 선택적 소프트 클리퍼. float로 처리해서 int16 포화를 피함.
// clip = true면 y = c * tanh(x / c): 작은 신호는 그대로, 큰 신호는 c로 부드럽게 눌림.
class GainStage : public AudioStream {
public:
  explicit GainStage(bool clip) : AudioStream(1, inputQueue), clip_(clip) {}
  void setGainDb(float db) { target_ = powf(10.0f, db / 20.0f); }  // 32비트 float 쓰기는 원자적

  void update() override {
    audio_block_t* b = receiveWritable(0);
    if (!b) return;
    float g = cur_;
    const float step = (target_ - g) / AUDIO_BLOCK_SAMPLES;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      float x = b->data[i] * (1.0f / 32768.0f) * g;
      g += step;
      if (clip_) x = CLIP_CEIL * tanhf(x / CLIP_CEIL);
      if (x > 1.0f) x = 1.0f;
      if (x < -1.0f) x = -1.0f;
      b->data[i] = (int16_t)(x * 32767.0f);
    }
    cur_ = g;
    transmit(b);
    release(b);
  }

private:
  audio_block_t* inputQueue[1];
  const bool clip_;
  volatile float target_ = 1.0f;
  float cur_ = 1.0f;
};

AudioInputI2S       audioIn;
AudioAnalyzePeak    peakIn;
GainStage           drive(true);   // Gain + 소프트 클리퍼 (NAM 자리)
AudioFilterBiquad   eq;            // 단 0 = Bass, 1 = Mid, 2 = Treble
GainStage           master(false);
AudioEffectFade     fadeWet;
AudioEffectFade     fadeDry;
AudioMixer4         mix;
AudioAnalyzePeak    peakOut;
AudioOutputI2S      audioOut;

AudioConnection c1(audioIn, 0, peakIn, 0);
AudioConnection c2(audioIn, 0, drive, 0);
AudioConnection c3(drive, 0, eq, 0);
AudioConnection c4(eq, 0, master, 0);
AudioConnection c5(master, 0, fadeWet, 0);
AudioConnection c6(audioIn, 0, fadeDry, 0);
AudioConnection c7(fadeWet, 0, mix, 0);
AudioConnection c8(fadeDry, 0, mix, 1);
AudioConnection c9(mix, 0, peakOut, 0);
AudioConnection c10(mix, 0, audioOut, 0);
AudioConnection c11(mix, 0, audioOut, 1);
Codec48k codec;
int lineInLevel = 0;  // 0 = 3.12Vpp에서 시작 (hardware-design.md 3-1절)

// ================= 화면 =================
U8G2_SSD1305_128X32_ADAFRUIT_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
// U8G2_SSD1305_128X32_NONAME_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);

#define SCREEN_W     128
#define UI_FONT      u8g2_font_6x10_tf
#define POPUP_FONT   u8g2_font_10x20_tf
const uint8_t LINE_Y[3] = {7, 18, 29};
#define LINE_PRESET  0
#define LINE_LABEL   1
#define LINE_VALUE   2
#define KNOB_AREA_W  90
#define METER_X      92
#define METER_BAR_X  (METER_X + 7)
#define METER_BAR_W  (SCREEN_W - METER_BAR_X)
#define METER_H      7

const uint16_t FRAME_CHOICES[3] = {50, 200, 1000};  // 20 / 5 / 1 fps ('f'로 순환)
int frameChoice = 0;
bool screenOn = true;
bool statsOn = true;

// ================= 파라미터·페이지·프리셋 =================
enum ParamId { P_BASS, P_MID, P_TREBLE, P_GAIN, P_GATE, P_MASTER, P_COUNT };
const char* const PARAM_NAME[P_COUNT]  = {"Bass", "Mid", "Treble", "Gain", "Gate", "Master"};
const char* const PARAM_SHORT[P_COUNT] = {"Bass", "Mid", "Treb", "Gain", "Gate", "Mstr"};

struct KnobPage { const char* name; ParamId pot[3]; };
const KnobPage PAGES[] = {
  {"EQ",     {P_BASS, P_MID, P_TREBLE}},
  {"IN/OUT", {P_GAIN, P_GATE, P_MASTER}},
};
const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

// 노브 0~1 ↔ dB. 0dB 위치: Gain 12/36 = 0.333, Master 40/46 = 0.870, EQ 0.5
#define GAIN_0DB    ((0.0f - GAIN_MIN_DB) / (GAIN_MAX_DB - GAIN_MIN_DB))
#define MASTER_0DB  ((0.0f - MASTER_MIN_DB) / (MASTER_MAX_DB - MASTER_MIN_DB))

struct Preset { const char* name; float p[P_COUNT]; };  // ParamId 순서
const Preset PRESETS[] = {
  {"Flat Test",    {0.50f, 0.50f, 0.50f, GAIN_0DB, 0.00f, MASTER_0DB}},  // 통과 기준 1용
  {"Plexi Crunch", {0.50f, 0.45f, 0.71f, 0.62f,    0.30f, 0.80f}},
  {"Clean Twin",   {0.40f, 0.55f, 0.60f, 0.25f,    0.20f, 0.85f}},
  {"Lead 5150",    {0.60f, 0.35f, 0.65f, 0.85f,    0.45f, 0.75f}},
};
const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// ================= 상태 =================
float params[P_COUNT];
int presetIdx = 0;
int page = 0;
bool bypass = false;

bool previewing = false;
int previewIdx = 0;
unsigned long previewAt = 0;

int popupParam = -1;
unsigned long popupAt = 0;

const uint8_t potPins[3] = {PIN_POT1, PIN_POT2, PIN_POT3};
float potPos[3] = {-1, -1, -1};
bool potLocked[3] = {true, true, true};
unsigned long potLastReadAt = 0;

Encoder enc(PIN_ENC_A, PIN_ENC_B);
long encBase = 0;

enum ButtonEvent { BTN_NONE, BTN_SHORT, BTN_LONG };
struct Button {
  uint8_t pin;
  bool raw;
  unsigned long rawSince;
  bool pressed;
  unsigned long pressedAt;
  bool longFired;
};
Button encBtn  = {PIN_ENC_SW};
Button footBtn = {PIN_FOOTSW};

// 미터 상태 (화면 프레임마다 갱신)
struct Meter {
  float level = 0;        // 0~1 (dBFS를 METER_FLOOR_DB~0으로 환산)
  float hold = 0;
  unsigned long holdAt = 0;
  unsigned long clipAt = 0;
  bool everClipped = false;
  float maxDb = -120;     // 통계 구간 최대
  uint32_t clips = 0;     // 통계 구간 CLIP 횟수 (프레임 단위)
};
Meter mIn, mOut;

unsigned long lastFrameAt = 0, lastReportAt = 0;
uint32_t sendSumUs = 0, sendMaxUs = 0, sendFrames = 0, loopMaxUs = 0;

// ================= DSP 파라미터 적용 (loop에서 계산, 오디오에는 결과만) =================
float eqDb(float v) { return (v - 0.5f) * 2.0f * EQ_RANGE_DB; }

// RBJ Audio EQ Cookbook 피킹. 라이브러리가 a1, a2 부호를 뒤집어 저장하므로 표준 형태로 넘긴다.
void setPeaking(int stage, float hz, float q, float db) {
  double A = pow(10.0, db / 40.0);
  double w0 = 2.0 * M_PI * hz / AUDIO_SAMPLE_RATE_EXACT;
  double alpha = sin(w0) / (2.0 * q);
  double a0 = 1.0 + alpha / A;
  double c[5] = {(1.0 + alpha * A) / a0, (-2.0 * cos(w0)) / a0, (1.0 - alpha * A) / a0,
                 (-2.0 * cos(w0)) / a0, (1.0 - alpha / A) / a0};
  eq.setCoefficients(stage, c);
}

void applyParam(int id) {
  switch (id) {
    case P_BASS:   eq.setLowShelf(0, EQ_BASS_HZ, eqDb(params[P_BASS]), 1.0f); break;
    case P_MID:    setPeaking(1, EQ_MID_HZ, EQ_MID_Q, eqDb(params[P_MID])); break;
    case P_TREBLE: eq.setHighShelf(2, EQ_TREBLE_HZ, eqDb(params[P_TREBLE]), 1.0f); break;
    case P_GAIN:   drive.setGainDb(GAIN_MIN_DB + params[P_GAIN] * (GAIN_MAX_DB - GAIN_MIN_DB)); break;
    case P_MASTER: master.setGainDb(MASTER_MIN_DB + params[P_MASTER] * (MASTER_MAX_DB - MASTER_MIN_DB)); break;
    case P_GATE:   break;  // 표시만 (처리 없음)
  }
}

void setBypass(bool on) {
  bypass = on;
  if (on) { fadeWet.fadeOut(BYPASS_FADE_MS); fadeDry.fadeIn(BYPASS_FADE_MS); }
  else    { fadeDry.fadeOut(BYPASS_FADE_MS); fadeWet.fadeIn(BYPASS_FADE_MS); }
  digitalWrite(PIN_LED, on ? LOW : HIGH);  // LED 켜짐 = 이펙트 켜짐
  Serial.printf("%s (LED %s)\n", on ? "바이패스" : "이펙트 켜짐", on ? "OFF" : "ON");
}

void setLineIn(int lvl) {
  lineInLevel = constrain(lvl, 0, 15);
  codec.lineInLevel(lineInLevel);
  mIn.everClipped = false;
  Serial.printf("lineInLevel %d (풀스케일 %.2fVpp)\n", lineInLevel, LINE_IN_VPP[lineInLevel]);
}

// ================= 입력 (08과 같음) =================
void buttonBegin(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.raw = digitalRead(b.pin) == LOW;
  b.pressed = b.raw;
  b.rawSince = b.pressedAt = millis();
  b.longFired = b.pressed;
}

ButtonEvent buttonPoll(Button& b, unsigned long now) {
  bool r = digitalRead(b.pin) == LOW;
  if (r != b.raw) { b.raw = r; b.rawSince = now; }
  if (b.raw != b.pressed && now - b.rawSince >= DEBOUNCE_MS) {
    b.pressed = b.raw;
    if (b.pressed) { b.pressedAt = b.rawSince; b.longFired = false; }
    else if (!b.longFired) return BTN_SHORT;
  }
  if (b.pressed && !b.longFired && now - b.pressedAt >= LONG_PRESS_MS) {
    b.longFired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

int encoderSteps() {
  long raw = enc.read();
  int steps = 0;
  while (raw - encBase >= ENC_COUNTS_PER_DETENT) { encBase += ENC_COUNTS_PER_DETENT; steps++; }
  while (encBase - raw >= ENC_COUNTS_PER_DETENT) { encBase -= ENC_COUNTS_PER_DETENT; steps--; }
  return steps;
}

void lockPots() { for (int i = 0; i < 3; i++) potLocked[i] = true; }

void loadPreset(int idx) {
  presetIdx = idx;
  for (int i = 0; i < P_COUNT; i++) { params[i] = PRESETS[idx].p[i]; applyParam(i); }
  lockPots();
  Serial.printf("프리셋 로드: [%02d] %s (포트 잠금)\n", idx + 1, PRESETS[idx].name);
}

bool potsOn = USE_POTS;

void pollPots(unsigned long now) {
  if (!potsOn) return;
  if (now - potLastReadAt < POT_INTERVAL_MS) return;
  potLastReadAt = now;
  for (int i = 0; i < 3; i++) {
    float pos = analogRead(potPins[i]) / 4095.0f;
    float prev = potPos[i];
    if (prev >= 0 && fabsf(pos - prev) < POT_HYST) continue;
    potPos[i] = pos;
    if (prev < 0) continue;

    ParamId id = PAGES[page].pot[i];
    if (potLocked[i]) {
      float v = params[id];
      if (fabsf(pos - v) <= CATCH_WINDOW || (prev - v) * (pos - v) <= 0) {
        potLocked[i] = false;
        Serial.printf("POT%d 잠금 해제 (%s)\n", i + 1, PARAM_NAME[id]);
      } else {
        continue;
      }
    }
    params[id] = pos;
    applyParam(id);
    popupParam = id;
    popupAt = now;
  }
}

void handleInputs(unsigned long now) {
  pollPots(now);

  int steps = encoderSteps();
  if (steps) {
    if (!previewing) { previewing = true; previewIdx = presetIdx; }
    previewIdx = ((previewIdx + steps) % PRESET_COUNT + PRESET_COUNT) % PRESET_COUNT;
    previewAt = now;
    popupParam = -1;
  }
  if (previewing && now - previewAt >= PREVIEW_MS) previewing = false;

  ButtonEvent e = buttonPoll(encBtn, now);
  if (e == BTN_SHORT) {
    if (previewing) { previewing = false; loadPreset(previewIdx); }
    else {
      page = (page + 1) % PAGE_COUNT;
      lockPots();
      Serial.printf("노브 페이지: %s (포트 잠금)\n", PAGES[page].name);
    }
  } else if (e == BTN_LONG) {
    Serial.println("MENU (이 스케치에는 없음)");
  }

  e = buttonPoll(footBtn, now);
  if (e == BTN_SHORT) setBypass(!bypass);
  else if (e == BTN_LONG) { previewing = false; loadPreset((presetIdx + 1) % PRESET_COUNT); }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'o':
        screenOn = !screenOn;
        if (!screenOn) { oled.clearBuffer(); oled.sendBuffer(); }
        Serial.printf("화면 갱신 %s\n", screenOn ? "켜짐" : "꺼짐");
        break;
      case 'f':
        frameChoice = (frameChoice + 1) % 3;
        Serial.printf("프레임 %d fps\n", 1000 / FRAME_CHOICES[frameChoice]);
        break;
      case 'b': setBypass(!bypass); break;
      case 'l': setLineIn(lineInLevel - 1); break;  // 번호 내림 = 더 큰 입력 허용
      case 'L': setLineIn(lineInLevel + 1); break;  // 번호 올림 = 더 민감
      case 's':
        statsOn = !statsOn;
        Serial.printf("통계 출력 %s\n", statsOn ? "켜짐" : "꺼짐");
        break;
      case 'p':
        potsOn = !potsOn;
        for (int i = 0; i < 3; i++) { potPos[i] = -1.0f; potLocked[i] = true; }  // 다시 켤 때 캐치부터
        Serial.printf("포트 읽기 %s\n", potsOn ? "켜짐 (잠금 상태로 시작)" : "꺼짐 (프리셋 값 고정)");
        break;
      case 'n': previewing = false; loadPreset((presetIdx + 1) % PRESET_COUNT); break;
      case 'c': {
        static const uint8_t CONTRASTS[] = {255, 128, 32, 1};
        static int ci = 0;
        ci = (ci + 1) % 4;
        oled.setContrast(CONTRASTS[ci]);
        Serial.printf("화면 밝기 %d\n", CONTRASTS[ci]);
        break;
      }
      default: break;
    }
  }
}

// ================= 미터 =================
void updateMeter(Meter& m, AudioAnalyzePeak& pk, unsigned long now) {
  if (!pk.available()) return;
  float p = pk.read();
  float db = p > 0.00001f ? 20.0f * log10f(p) : -100.0f;
  if (db > m.maxDb) m.maxDb = db;
  if (db >= CLIP_DB) { m.clipAt = now; m.everClipped = true; m.clips++; }
  float lv = (db - METER_FLOOR_DB) / -METER_FLOOR_DB;
  m.level = constrain(lv, 0.0f, 1.0f);
  if (m.level >= m.hold) { m.hold = m.level; m.holdAt = now; }
  else if (now - m.holdAt > PEAK_HOLD_MS) m.hold = max(m.level, m.hold - 0.02f);
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

void fmtValue(char* buf, size_t n, float v, bool locked) {
  char num[6];
  if (v * 10 >= 9.95f) snprintf(num, sizeof(num), "10");
  else snprintf(num, sizeof(num), "%.1f", v * 10);
  snprintf(buf, n, locked ? "(%s)" : "%s", num);
}

// 미터 한 줄: 라벨(I/O, CLIP이면 반전) + 막대 + 피크 홀드 세로선
void drawMeterRow(const Meter& m, const char* label, int baseline, unsigned long now) {
  int top = baseline - METER_H + 1;
  if (now - m.clipAt < CLIP_SHOW_MS) {
    oled.drawBox(METER_X, top, 6, METER_H);
    oled.setDrawColor(0);
    oled.drawStr(METER_X, baseline, label);
    oled.setDrawColor(1);
  } else {
    oled.drawStr(METER_X, baseline, label);
  }
  oled.drawFrame(METER_BAR_X, top, METER_BAR_W, METER_H);
  int inner = METER_BAR_W - 2;
  int fill = (int)(inner * m.level + 0.5f);
  if (fill > 0) oled.drawBox(METER_BAR_X + 1, top + 1, fill, METER_H - 2);
  int hx = METER_BAR_X + 1 + (int)(inner * m.hold + 0.5f);
  if (hx > METER_BAR_X + inner) hx = METER_BAR_X + inner;
  oled.drawVLine(hx, top + 1, METER_H - 2);
}

void drawHome(unsigned long now) {
  oled.setFont(UI_FONT);

  int idx = previewing ? previewIdx : presetIdx;
  char buf[24];
  snprintf(buf, sizeof(buf), "[%02d] %s", idx + 1, PRESETS[idx].name);
  if (previewing) { oled.drawBox(0, 0, SCREEN_W, 9); oled.setDrawColor(0); }
  oled.drawStr(0, LINE_Y[LINE_PRESET], buf);
  if (bypass) oled.drawStr(rightX("BYP"), LINE_Y[LINE_PRESET], "BYP");
  oled.setDrawColor(1);

  const int colW = KNOB_AREA_W / 3;
  char v[8];
  for (int i = 0; i < 3; i++) {
    ParamId id = PAGES[page].pot[i];
    drawCentered(i * colW, colW, LINE_Y[LINE_LABEL], PARAM_SHORT[id]);
    fmtValue(v, sizeof(v), params[id], potLocked[i]);
    drawCentered(i * colW, colW, LINE_Y[LINE_VALUE], v);
  }

  drawMeterRow(mIn, "I", LINE_Y[LINE_LABEL], now);
  drawMeterRow(mOut, "O", LINE_Y[LINE_VALUE], now);
}

void drawPopup() {
  float v = params[popupParam];
  char buf[8];
  fmtValue(buf, sizeof(buf), v, false);
  oled.setFont(UI_FONT);
  oled.drawStr(0, 9, PARAM_NAME[popupParam]);
  if (popupParam == P_GATE) oled.drawStr(0, 21, "(no DSP)");  // 처리 없음 표시
  oled.setFont(POPUP_FONT);
  oled.drawStr(rightX(buf), 22, buf);
  oled.drawFrame(0, 26, SCREEN_W, 6);
  int fill = (int)((SCREEN_W - 2) * v + 0.5f);
  if (fill > 0) oled.drawBox(1, 27, fill, 4);
}

void drawScreen(unsigned long now) {
  oled.clearBuffer();
  oled.setFontMode(1);
  if (popupParam >= 0 && now - popupAt < POPUP_MS) drawPopup();
  else { popupParam = -1; drawHome(now); }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}

  AudioMemory(24);
  codec.enable();
  bool fsOk = codec.setSysFs48k();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineOutLevel(LINE_OUT_LEVEL);
  codec.volume(HEADPHONE_VOL);
  mix.gain(0, 1.0f);
  mix.gain(1, 1.0f);
  fadeDry.fadeOut(0);
  fadeWet.fadeIn(0);

  analogReadResolution(12);
  analogReadAveraging(16);
  buttonBegin(encBtn);
  buttonBegin(footBtn);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  oled.begin();

  Serial.println("09_breadboard_system 시작");
  Serial.printf("샘플레이트 %.1f Hz (SYS_FS 48k 설정 %s), 블록 %d 샘플\n",
                AUDIO_SAMPLE_RATE_EXACT, fsOk ? "성공" : "실패", AUDIO_BLOCK_SAMPLES);
  setLineIn(0);
  Serial.println("명령: o 화면 | f fps | c 밝기 | b 바이패스 | l/L lineInLevel | s 통계 | p 포트 | n 다음 프리셋");
  if (!potsOn) Serial.println("포트 읽기 꺼짐 (USE_POTS 0). 노브 값은 프리셋 값으로 고정");
  loadPreset(0);
  lastReportAt = millis();
}

void loop() {
  uint32_t loopStart = micros();
  unsigned long now = millis();

  handleSerial();
  handleInputs(now);

  if (now - lastFrameAt >= FRAME_CHOICES[frameChoice]) {
    lastFrameAt = now;
    updateMeter(mIn, peakIn, now);
    updateMeter(mOut, peakOut, now);
    if (screenOn) {
      drawScreen(now);
      uint32_t t0 = micros();
      oled.sendBuffer();
      uint32_t dt = micros() - t0;
      sendSumUs += dt;
      if (dt > sendMaxUs) sendMaxUs = dt;
      sendFrames++;
    }
  }

  if (now - lastReportAt >= REPORT_MS) {
    lastReportAt = now;
    if (statsOn) {
      Serial.printf("[통계] CPU %.1f%% (최대 %.1f%%), 메모리 최대 %d | IN 최대 %.1f dBFS 클립 %lu%s | "
                    "OUT 최대 %.1f dBFS 클립 %lu | 전송 평균 %lu / 최대 %lu us | 루프 최대 %lu us\n",
                    AudioProcessorUsage(), AudioProcessorUsageMax(), AudioMemoryUsageMax(),
                    mIn.maxDb, (unsigned long)mIn.clips, mIn.everClipped ? " (클립 있었음)" : "",
                    mOut.maxDb, (unsigned long)mOut.clips,
                    sendFrames ? (unsigned long)(sendSumUs / sendFrames) : 0UL, (unsigned long)sendMaxUs,
                    (unsigned long)loopMaxUs);
    }
    mIn.maxDb = mOut.maxDb = -120;
    mIn.clips = mOut.clips = 0;
    sendSumUs = sendMaxUs = sendFrames = loopMaxUs = 0;
  }

  uint32_t lt = micros() - loopStart;
  if (lt > loopMaxUs) loopMaxUs = lt;
}
