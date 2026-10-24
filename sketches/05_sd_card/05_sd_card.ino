// 05_sd_card: Teensy 4.1 내장 microSD 슬롯 읽기/쓰기 확인
// SD 카드 용량, /models 파일 목록, 테스트 파일 쓰기/읽기 결과와 4KB 청크 읽기 속도가 시리얼에 찍히면 통과.
// SD 카드는 FAT32로 포맷하고, 저장소의 sd-card/ 폴더 내용을 그대로 복사해 두세요.

#include <SD.h>

void listDir(const char* path) {
  File dir = SD.open(path);
  if (!dir) {
    Serial.printf("%s 폴더 없음\n", path);
    return;
  }
  Serial.printf("[%s]\n", path);
  while (File f = dir.openNextFile()) {
    Serial.printf("  %-32s %8lu bytes\n", f.name(), (unsigned long)f.size());
    f.close();
  }
  dir.close();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println("05_sd_card 시작");

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD 카드 인식 실패: 카드가 꽂혀 있는지, FAT32인지 확인");
    return;
  }
  Serial.printf("SD 용량: %llu MB\n", SD.totalSize() / (1024 * 1024));

  listDir("/models");
  listDir("/presets");
  listDir("/system");

  // 쓰기/읽기 테스트
  const char* testPath = "/system/sd_test.txt";
  SD.remove(testPath);
  File w = SD.open(testPath, FILE_WRITE);
  w.println("teensy-nam-pedal sd test");
  w.close();
  File r = SD.open(testPath);
  Serial.print("읽은 내용: ");
  while (r.available()) Serial.write(r.read());
  r.close();

  // 4KB 청크 읽기 속도 (ModelLoader가 loop() 1회에 4KB씩 읽는 구조라 1청크 시간이 중요)
  File big = SD.open("/system/speed_test.bin", FILE_WRITE);
  static uint8_t buf[4096];
  for (int i = 0; i < 64; i++) big.write(buf, sizeof(buf));  // 256KB
  big.close();
  big = SD.open("/system/speed_test.bin");
  unsigned long worst = 0, t0 = micros();
  while (big.available()) {
    unsigned long c0 = micros();
    big.read(buf, sizeof(buf));
    worst = max(worst, micros() - c0);
  }
  unsigned long total = micros() - t0;
  big.close();
  SD.remove("/system/speed_test.bin");
  Serial.printf("256KB 읽기: %lu us, 4KB 청크 최악 %lu us\n", total, worst);
}

void loop() {}
