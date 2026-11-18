// 03_knobs_switches: 포트 3개, EC11 인코더(푸시), 풋스위치, 상태 LED 입력 확인 (H2)
// 저장소 루트에서:  pio run -d sketches/03_knobs_switches -t upload
// 시리얼 모니터:   pio device monitor -b 115200
//
// [통과 기준]
// - 포트 3개 모두 끝에서 끝까지 0~100% 근처로 움직이고, 손을 떼면 값이 떨리지 않음
// - 인코더 한 칸(딸깍 1번)에 1씩 바뀜
// - 인코더 버튼과 풋스위치가 짧게/길게를 구분함, 길게는 누른 지 600ms에 (떼기 전에) 바로 찍힘
// - 풋스위치를 짧게 누를 때마다 LED가 토글됨 (바이패스 표시 흉내)
//
// [출력 예시]
// POT1 42%
// ENC 7
// 풋스위치 짧게 → LED ON
// 인코더 버튼 길게

#include <Arduino.h>
#include <Encoder.h>

// ---- 핀 배치 (firmware/src/pins.h, docs/hardware-design.md 5장과 같게 유지) ----
// 13번은 OLED SCK + 보드 LED 공용이라 쓰지 않는다.
#define PIN_POT1    14  // A0, 와이퍼 → 1kΩ → 핀, 핀에 0.1µF to GND
#define PIN_POT2    15  // A1
#define PIN_POT3    16  // A2
#define PIN_ENC_A   2   // 공통 핀 GND, 각 핀 10nF to GND
#define PIN_ENC_B   3
#define PIN_ENC_SW  4   // 인코더 푸시, 누르면 GND
#define PIN_FOOTSW  5   // 모멘터리, 누르면 GND, 100nF 병렬
#define PIN_LED     9   // 9 → 1kΩ → LED → GND

// ---- 동작 상수 ----
#define DEBOUNCE_MS      20    // 입력이 이 시간 동안 안 바뀌어야 상태 확정
#define LONG_PRESS_MS    600   // 누른 지 이 시간이 되면 즉시 "길게"
#define POT_HYST         1.0f  // % 단위. 마지막 출력값과 1% 이상 차이 날 때만 출력
#define POT_INTERVAL_MS  10    // 포트 읽기 주기 (매 루프마다 읽을 필요 없음)

// EC11은 디텐트 한 칸에 Encoder 카운트 4. 한 칸에 2씩 바뀌면(반 디텐트형 부품) 2로 바꿀 것.
#define ENC_COUNTS_PER_DETENT  4

// ---- 포트 ----
const uint8_t potPins[3] = {PIN_POT1, PIN_POT2, PIN_POT3};
float potShown[3] = {-100, -100, -100};  // 마지막으로 출력한 값(%). 처음엔 무조건 출력되게
unsigned long potLastReadAt = 0;

// ---- 인코더 ----
Encoder enc(PIN_ENC_A, PIN_ENC_B);
long encBase = 0;   // 마지막으로 칸을 센 위치의 원시 카운트
long encValue = 0;  // 디텐트 단위 값

// ---- 버튼 (인코더 버튼, 풋스위치 공용) ----
enum ButtonEvent { BTN_NONE, BTN_SHORT, BTN_LONG };

struct Button {
  uint8_t pin;
  const char* name;
  bool raw;                 // 마지막 원시 읽기 (true = 눌림)
  unsigned long rawSince;   // 원시 값이 마지막으로 바뀐 시각
  bool pressed;             // 디바운스 후 확정 상태
  unsigned long pressedAt;  // 눌림이 시작된 시각 (원시 기준)
  bool longFired;           // 이번 누름에서 "길게"를 이미 냈는지
};

Button encBtn  = {PIN_ENC_SW, "인코더 버튼"};
Button footBtn = {PIN_FOOTSW, "풋스위치"};
bool ledOn = false;

void buttonBegin(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);  // 외부 풀업 없음
  b.raw = digitalRead(b.pin) == LOW;
  b.pressed = b.raw;
  b.rawSince = b.pressedAt = millis();
  b.longFired = b.pressed;  // 켤 때 이미 눌려 있으면 이벤트 안 냄
}

// 짧게: 뗄 때 판정 / 길게: 누른 지 LONG_PRESS_MS가 되는 순간 판정. 길게가 났으면 뗄 때 짧게 없음.
ButtonEvent buttonPoll(Button& b, unsigned long now) {
  bool r = digitalRead(b.pin) == LOW;  // 풀업이라 누르면 LOW
  if (r != b.raw) {
    b.raw = r;
    b.rawSince = now;
  }

  // 원시 값이 DEBOUNCE_MS 동안 그대로면 상태 확정
  if (b.raw != b.pressed && now - b.rawSince >= DEBOUNCE_MS) {
    b.pressed = b.raw;
    if (b.pressed) {
      b.pressedAt = b.rawSince;  // 실제로 눌린 시각부터 600ms를 센다
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

void pollPots(unsigned long now) {
  if (now - potLastReadAt < POT_INTERVAL_MS) return;
  potLastReadAt = now;

  for (int i = 0; i < 3; i++) {
    float pct = analogRead(potPins[i]) * 100.0f / 4095.0f;
    float diff = fabsf(pct - potShown[i]);
    // 양끝(0%/100%)은 히스테리시스 때문에 1% 직전에서 멈추지 않도록 따로 맞춘다
    bool atEdge = (pct < 0.5f && potShown[i] >= 0.5f) || (pct > 99.5f && potShown[i] <= 99.5f);
    if (diff >= POT_HYST || atEdge) {
      potShown[i] = pct;
      Serial.printf("POT%d %d%%\n", i + 1, (int)(pct + 0.5f));
    }
  }
}

void pollEncoder() {
  long raw = enc.read();
  bool changed = false;
  // 카운트가 한 칸(ENC_COUNTS_PER_DETENT)만큼 쌓일 때마다 1씩. 0 근처에서도 양방향 대칭.
  while (raw - encBase >= ENC_COUNTS_PER_DETENT) {
    encBase += ENC_COUNTS_PER_DETENT;
    encValue++;
    changed = true;
  }
  while (encBase - raw >= ENC_COUNTS_PER_DETENT) {
    encBase -= ENC_COUNTS_PER_DETENT;
    encValue--;
    changed = true;
  }
  if (changed) Serial.printf("ENC %ld\n", encValue);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}  // 시리얼 모니터 연결을 최대 2초 기다림

  analogReadResolution(12);
  analogReadAveraging(16);
  buttonBegin(encBtn);
  buttonBegin(footBtn);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial.println("03_knobs_switches 시작");
  Serial.println("핀 배치:");
  Serial.printf("  포트1/2/3     : %d(A0) / %d(A1) / %d(A2)\n", PIN_POT1, PIN_POT2, PIN_POT3);
  Serial.printf("  인코더 A/B/SW : %d / %d / %d (INPUT_PULLUP)\n", PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW);
  Serial.printf("  풋스위치      : %d (INPUT_PULLUP)\n", PIN_FOOTSW);
  Serial.printf("  상태 LED      : %d\n", PIN_LED);
  Serial.printf("설정: 디바운스 %dms, 길게 %dms, 포트 히스테리시스 %.0f%%, 인코더 %d카운트/칸\n",
                DEBOUNCE_MS, LONG_PRESS_MS, POT_HYST, ENC_COUNTS_PER_DETENT);
}

void loop() {
  unsigned long now = millis();

  pollPots(now);
  pollEncoder();

  ButtonEvent e = buttonPoll(encBtn, now);
  if (e == BTN_SHORT) Serial.printf("%s 짧게\n", encBtn.name);
  if (e == BTN_LONG)  Serial.printf("%s 길게\n", encBtn.name);

  e = buttonPoll(footBtn, now);
  if (e == BTN_SHORT) {
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
    Serial.printf("%s 짧게 → LED %s\n", footBtn.name, ledOn ? "ON" : "OFF");
  }
  if (e == BTN_LONG) Serial.printf("%s 길게\n", footBtn.name);
}
