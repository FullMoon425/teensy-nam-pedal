// 03_knobs_switches: 포트 3개, 인코더(푸시), 풋스위치, LED 입력 확인
// 노브를 돌리면 0~100% 값이, 인코더를 돌리면 카운트가, 스위치를 누르면 짧게/길게 이벤트가 시리얼에 찍히면 통과.
// 풋스위치를 짧게 누를 때마다 LED가 토글됩니다(바이패스 표시 흉내).

#include <Encoder.h>

// 핀 배치 (docs/hardware-design.md 5장 초안, firmware/src/pins.h와 동일하게 유지)
#define PIN_POT1     14  // A0
#define PIN_POT2     15  // A1
#define PIN_POT3     16  // A2
#define PIN_ENC_A    2
#define PIN_ENC_B    3
#define PIN_ENC_SW   4
#define PIN_FOOTSW   5
#define PIN_LED      9

#define LONG_PRESS_MS  600
#define DEBOUNCE_MS    20
#define POT_HYST       0.01f  // 1% 이상 바뀔 때만 출력

Encoder enc(PIN_ENC_A, PIN_ENC_B);

struct Button {
  uint8_t pin;
  const char* name;
  bool pressed = false;
  bool longFired = false;
  unsigned long changedAt = 0;
};

Button encBtn{PIN_ENC_SW, "인코더 버튼"};
Button footBtn{PIN_FOOTSW, "풋스위치"};

const uint8_t potPins[3] = {PIN_POT1, PIN_POT2, PIN_POT3};
float potLast[3] = {-1, -1, -1};
long encLast = 0;
bool ledOn = false;

// 짧게: 뗄 때 판정 / 길게: 600ms 도달 즉시 판정 (software-design.md 8-1절)
// 반환값: 0 = 없음, 1 = 짧게, 2 = 길게
int pollButton(Button& b) {
  bool down = digitalRead(b.pin) == LOW;  // 내부 풀업, 누르면 LOW
  unsigned long now = millis();
  if (down != b.pressed && now - b.changedAt >= DEBOUNCE_MS) {
    b.pressed = down;
    b.changedAt = now;
    if (down) {
      b.longFired = false;
    } else if (!b.longFired) {
      return 1;
    }
  }
  if (b.pressed && !b.longFired && now - b.changedAt >= LONG_PRESS_MS) {
    b.longFired = true;
    return 2;
  }
  return 0;
}

void report(Button& b, int evt) {
  if (evt) Serial.printf("%s: %s\n", b.name, evt == 1 ? "짧게" : "길게");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_FOOTSW, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  analogReadResolution(12);
  analogReadAveraging(16);
  Serial.println("03_knobs_switches 시작");
}

void loop() {
  for (int i = 0; i < 3; i++) {
    float v = analogRead(potPins[i]) / 4095.0f;
    if (fabsf(v - potLast[i]) > POT_HYST) {
      potLast[i] = v;
      Serial.printf("포트%d: %5.1f%%\n", i + 1, v * 100);
    }
  }

  long e = enc.read() / 4;  // EC11: 디텐트 1칸 = 4카운트 (확인 필요: 부품에 따라 2일 수 있음)
  if (e != encLast) {
    Serial.printf("인코더: %ld (%s)\n", e, e > encLast ? "시계방향" : "반시계방향");
    encLast = e;
  }

  report(encBtn, pollButton(encBtn));

  int f = pollButton(footBtn);
  report(footBtn, f);
  if (f == 1) {
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn);
  }

  delay(1);
}
