// 05_sd_card: Teensy 4.1 내장 microSD 슬롯 읽기/쓰기 확인
// SD 카드 용량, /models 파일 목록, 테스트 파일 쓰기/읽기 결과와 4KB 청크 읽기 속도가 시리얼에 찍히면 통과.
// SD 카드는 FAT32로 포맷하고, 저장소의 sd-card/ 폴더 내용을 그대로 복사해 두세요.
//
// 방금 쓴 파일은 연속된 클러스터에 놓이고 FAT 정보도 막 읽은 상태라 최악 시간이 낙관적으로 나올 수 있습니다.
// 그래서 /models에 미리 복사해 둔 파일이 있으면 그 파일(가장 큰 것)도 같은 방법으로 잽니다.
// 가장 현실적인 값은 전원을 껐다 켠 직후 첫 실행의 /models 결과입니다.

#include <SD.h>

static uint8_t buf[4096];

// 파일 전체를 4KB씩 읽으며 총 시간과 청크 최악 시간을 출력
void measureRead(const char* path, const char* label) {
  File f = SD.open(path);
  if (!f) {
    Serial.printf("%s 열기 실패\n", path);
    return;
  }
  unsigned long size = f.size();
  unsigned long worst = 0, t0 = micros();
  while (f.available()) {
    unsigned long c0 = micros();
    f.read(buf, sizeof(buf));
    worst = max(worst, micros() - c0);
  }
  unsigned long total = micros() - t0;
  f.close();
  Serial.printf("[%s] %s %luKB 읽기: %lu us, 4KB 청크 최악 %lu us\n",
                label, path, size / 1024, total, worst);
}

// /models에서 가장 큰 파일 경로를 out에 저장. 없으면 false
bool findLargestModel(char* out, size_t len) {
  File dir = SD.open("/models");
  if (!dir) return false;
  unsigned long best = 0;
  while (File f = dir.openNextFile()) {
    if (!f.isDirectory() && f.size() > best) {
      best = f.size();
      snprintf(out, len, "/models/%s", f.name());
    }
    f.close();
  }
  dir.close();
  return best > 0;
}

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
  // 1) 기존 파일: 쓰기 전에 먼저 재야 방금 쓴 데이터의 영향을 덜 받음
  char modelPath[64];
  if (findLargestModel(modelPath, sizeof(modelPath))) {
    measureRead(modelPath, "기존 파일");
  } else {
    Serial.println("/models에 파일이 없어 기존 파일 측정은 건너뜀 (아무 파일이나 넣어 두면 측정)");
  }

  // 2) 방금 쓴 파일 256KB (낙관적인 값)
  File big = SD.open("/system/speed_test.bin", FILE_WRITE);
  for (int i = 0; i < 64; i++) big.write(buf, sizeof(buf));
  big.close();
  measureRead("/system/speed_test.bin", "방금 쓴 파일");
  SD.remove("/system/speed_test.bin");
}

void loop() {}
