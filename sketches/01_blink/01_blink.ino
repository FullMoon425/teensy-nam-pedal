// 01_blink: Teensy 4.1 업로드 환경 확인
// 보드 LED(13번 핀)가 0.5초 간격으로 깜빡이고, 시리얼 모니터에 카운트가 찍히면 통과.

#define PIN_LED LED_BUILTIN  // Teensy 4.1 = 13번 핀

unsigned long count = 0;

void setup() {
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}  // 시리얼 모니터 연결을 최대 3초 기다림
  Serial.println("01_blink 시작");
  Serial.printf("CPU 클럭: %lu MHz\n", F_CPU_ACTUAL / 1000000);
}

void loop() {
  digitalWrite(PIN_LED, HIGH);
  delay(500);
  digitalWrite(PIN_LED, LOW);
  delay(500);
  Serial.printf("blink %lu\n", ++count);
}
