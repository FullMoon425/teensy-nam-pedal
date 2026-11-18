// 07_memory_test: Teensy 4.1 메모리 검증
// RAM1(DTCM), RAM2(OCRAM), PSRAM(장착 시)에 여러 패턴을 쓰고 다시 읽어 비교하고, 영역별 읽기/쓰기 속도를 잽니다.
// 시리얼에 모든 영역 "OK"가 나오고 반복 패스에서도 오류가 0이면 통과.
// 결과(특히 속도)는 NAM 가중치·상태 버퍼를 어디에 둘지 정할 때 참고합니다 (docs/plan.md: DTCM/OCRAM 배치).

#include <Arduino.h>

#define PIN_LED LED_BUILTIN

// 링커 심볼 (cores/teensy4/imxrt1062_t41.ld)
extern "C" {
extern unsigned long _stext, _etext, _sdata, _ebss, _heap_start, _heap_end, _estack;
extern unsigned long _extram_start, _extram_end, _flashimagelen;
extern uint8_t external_psram_size;  // startup.c가 부팅 때 감지한 PSRAM 크기(MB), 0이면 없음
}

// RAM1(DTCM) 시험용 정적 버퍼. DTCM은 캐시가 없고 CPU와 직결됩니다.
static const size_t DTCM_TEST_BYTES = 256 * 1024;
static uint32_t dtcmBuf[DTCM_TEST_BYTES / 4] __attribute__((aligned(32)));

static const int MAX_ERR_PRINT = 8;

// ---- 패턴 ----
enum Pattern { P_ADDR, P_NOT_ADDR, P_ZERO, P_ONES, P_55, P_AA, P_RANDOM, P_COUNT };
static const char *patternName[P_COUNT] = {"addr", "~addr", "0x00", "0xFF", "0x55", "0xAA", "random"};

static inline uint32_t xorshift(uint32_t &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

static inline uint32_t patternValue(Pattern p, volatile uint32_t *addr, uint32_t &rng) {
  switch (p) {
    case P_ADDR:     return (uint32_t)addr;
    case P_NOT_ADDR: return ~(uint32_t)addr;
    case P_ZERO:     return 0x00000000;
    case P_ONES:     return 0xFFFFFFFF;
    case P_55:       return 0x55555555;
    case P_AA:       return 0xAAAAAAAA;
    default:         return xorshift(rng);
  }
}

// 캐시가 있는 영역(RAM2, PSRAM)은 쓴 뒤 캐시를 비워서, 읽기가 캐시가 아니라 실제 메모리에서 오게 합니다.
static void flushIfCached(void *p, size_t bytes, bool cached) {
  if (cached) arm_dcache_flush_delete(p, bytes);
}

// 한 영역을 모든 패턴으로 검사. 반환값 = 틀린 워드 수
static uint32_t testRegion(const char *name, uint32_t *base, size_t bytes, bool cached, uint32_t seed) {
  volatile uint32_t *p = base;
  size_t words = bytes / 4;
  uint32_t errors = 0;
  int printed = 0;

  for (int pt = 0; pt < P_COUNT; pt++) {
    Pattern pat = (Pattern)pt;
    uint32_t rng = seed + pt * 0x9E3779B9u;
    if (rng == 0) rng = 1;
    for (size_t i = 0; i < words; i++) p[i] = patternValue(pat, &p[i], rng);
    flushIfCached(base, bytes, cached);

    rng = seed + pt * 0x9E3779B9u;
    if (rng == 0) rng = 1;
    uint32_t patErr = 0;
    for (size_t i = 0; i < words; i++) {
      uint32_t expect = patternValue(pat, &p[i], rng);
      uint32_t got = p[i];
      if (got != expect) {
        patErr++;
        if (printed < MAX_ERR_PRINT) {
          Serial.printf("    [%s/%s] 0x%08lX: 기대 0x%08lX, 읽음 0x%08lX\n", name, patternName[pt],
                        (uint32_t)&p[i], expect, got);
          printed++;
        }
      }
    }
    errors += patErr;
  }
  return errors;
}

// 순차 쓰기/읽기 속도(MB/s). 캐시 영역은 쓰기 뒤 flush까지 포함하고, 읽기 전에 캐시를 비웁니다.
static void benchRegion(uint32_t *base, size_t bytes, bool cached, float &wrMBs, float &rdMBs) {
  volatile uint32_t *p = base;
  size_t words = bytes / 4;

  flushIfCached(base, bytes, cached);
  uint32_t t0 = micros();
  for (size_t i = 0; i < words; i++) p[i] = i;
  flushIfCached(base, bytes, cached);
  uint32_t tw = micros() - t0;

  flushIfCached(base, bytes, cached);
  uint32_t sum = 0;
  t0 = micros();
  for (size_t i = 0; i < words; i++) sum += p[i];
  uint32_t tr = micros() - t0;

  wrMBs = tw ? (float)bytes / tw : 0;  // bytes/us == MB/s
  rdMBs = tr ? (float)bytes / tr : 0;
  if (sum == 0xDEADBEEF) Serial.print("");  // 읽기 루프가 최적화로 사라지지 않게
}

static void report(const char *name, uint32_t *base, size_t bytes, uint32_t errors, float wr, float rd) {
  Serial.printf("  %-6s 0x%08lX  %5u KB  쓰기 %6.1f MB/s  읽기 %6.1f MB/s  %s (오류 %lu)\n", name, (uint32_t)base,
                (unsigned)(bytes / 1024), wr, rd, errors ? "FAIL" : "OK", errors);
}

// RAM2 힙에서 한 번에 잡을 수 있는 가장 큰 블록 (4KB 단위)
static size_t largestMalloc() {
  // 4KB 페이지 수로 이분 탐색: lo는 항상 성공, hi는 항상 실패하는 크기
  size_t lo = 0, hi = ((uint32_t)&_heap_end - (uint32_t)&_heap_start) / 4096 + 1;
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    // volatile: -O2에서 GCC가 malloc/free 쌍을 "항상 성공"으로 보고 지워버리는 것을 막음
    void *volatile m = malloc(mid * 4096);
    if (m) { free(m); lo = mid; } else { hi = mid; }
  }
  return lo * 4096;
}

static void printMemoryMap() {
  uint32_t itcm = (uint32_t)&_etext - (uint32_t)&_stext;
  uint32_t itcmBlocks = (itcm + 0x7FFF) >> 15;
  uint32_t dtcmUsed = (uint32_t)&_ebss - (uint32_t)&_sdata;
  uint32_t sp;
  asm volatile("mov %0, sp" : "=r"(sp));
  uint32_t stackFree = sp - (uint32_t)&_ebss;
  uint32_t ram2Static = (uint32_t)&_heap_start - 0x20200000;
  uint32_t heapTotal = (uint32_t)&_heap_end - (uint32_t)&_heap_start;

  Serial.println("메모리 맵");
  Serial.printf("  플래시 이미지     %7lu B\n", (uint32_t)&_flashimagelen);
  Serial.printf("  ITCM(코드)        %7lu B  (%lu x 32KB 블록, RAM1 512KB 중)\n", itcm, itcmBlocks);
  Serial.printf("  DTCM(data+bss)    %7lu B  (시험 버퍼 %u KB 포함)\n", dtcmUsed, (unsigned)(DTCM_TEST_BYTES / 1024));
  Serial.printf("  DTCM 스택 여유    %7lu B  (스택 꼭대기 0x%08lX)\n", stackFree, (uint32_t)&_estack);
  Serial.printf("  RAM2 정적(DMAMEM) %7lu B\n", ram2Static);
  Serial.printf("  RAM2 힙           %7lu B\n", heapTotal);
  if (external_psram_size)
    Serial.printf("  PSRAM             %7u MB (EXTMEM 변수 %lu B)\n", external_psram_size,
                  (uint32_t)&_extram_end - (uint32_t)&_extram_start);
  else
    Serial.println("  PSRAM             없음 (패드에 칩 미장착 또는 인식 실패)");
}

static uint32_t pass = 0;
static uint32_t totalErrors = 0;

static void runPass() {
  pass++;
  uint32_t seed = 0x12345678u + pass * 7919u;
  uint32_t passErr = 0;
  uint32_t t0 = millis();
  float wr, rd;
  Serial.printf("\n=== 패스 %lu ===\n", pass);

  // RAM1 (DTCM)
  uint32_t e = testRegion("DTCM", dtcmBuf, DTCM_TEST_BYTES, false, seed);
  benchRegion(dtcmBuf, DTCM_TEST_BYTES, false, wr, rd);
  report("DTCM", dtcmBuf, DTCM_TEST_BYTES, e, wr, rd);
  passErr += e;

  // RAM2 (OCRAM): 힙에서 가장 큰 블록을 잡아 검사
  size_t ram2Bytes = largestMalloc() & ~31u;
  uint32_t *ram2 = ram2Bytes ? (uint32_t *)malloc(ram2Bytes) : nullptr;
  if (ram2) {
    e = testRegion("RAM2", ram2, ram2Bytes, true, seed);
    benchRegion(ram2, ram2Bytes, true, wr, rd);
    report("RAM2", ram2, ram2Bytes, e, wr, rd);
    passErr += e;
    free(ram2);
  } else {
    Serial.println("  RAM2   malloc 실패 - FAIL");
    passErr++;
  }

  // PSRAM: EXTMEM 변수 뒤부터 끝까지 전부 (이 스케치는 EXTMEM 변수가 없어 사실상 전체)
  if (external_psram_size) {
    uint32_t start = ((uint32_t)&_extram_end + 31) & ~31u;
    uint32_t end = 0x70000000u + external_psram_size * 0x100000u;
    uint32_t *ps = (uint32_t *)start;
    size_t psBytes = end - start;
    e = testRegion("PSRAM", ps, psBytes, true, seed);
    benchRegion(ps, psBytes, true, wr, rd);
    report("PSRAM", ps, psBytes, e, wr, rd);
    passErr += e;
  } else {
    Serial.println("  PSRAM  없음 - 건너뜀");
  }

  totalErrors += passErr;
  Serial.printf("패스 %lu: %s, %lu ms, 누적 오류 %lu\n", pass, passErr ? "FAIL" : "OK", millis() - t0, totalErrors);
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println("07_memory_test 시작");
  Serial.printf("CPU 클럭: %lu MHz\n", F_CPU_ACTUAL / 1000000);
  if (CrashReport) Serial.print(CrashReport);  // 직전 실행이 하드폴트로 재부팅됐으면 원인 출력
  printMemoryMap();
}

void loop() {
  digitalWrite(PIN_LED, HIGH);
  runPass();
  digitalWrite(PIN_LED, LOW);
  // 오류가 한 번이라도 나면 LED를 빠르게 깜빡여 표시
  for (int i = 0; i < 20; i++) {
    if (totalErrors) digitalToggle(PIN_LED);
    delay(100);
  }
  digitalWrite(PIN_LED, LOW);
}
