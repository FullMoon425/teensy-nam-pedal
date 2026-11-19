// 06_nam_benchmark (S0): 48kHz 설정 검증 + NAM 아키텍처별 CPU 벤치마크
//
// 1) 48kHz 검증: LINE OUT L로 1kHz 사인. 1000Hz면 정상, 약 1088Hz 또는 919Hz면 설정 불일치 (docs/software-design.md 3장)
// 2) 벤치마크: 더미 가중치 NAM 모델을 오디오 인터럽트 안에서 실제로 돌리고 블록당 사이클을 잼
//    모델 입력은 기본 110Hz 사인(측정값이 기타 연주와 무관하게 재현되도록), `i`로 LINE IN으로 바꿀 수 있음
//    모델 출력은 스피커로 보내지 않음 (더미 가중치라 의미 없는 소리)
// 3) 캐비닛 IR 컨볼루션(분할 FFT)을 모델 뒤에 붙여 비용을 따로 잼 (테스트용 가짜 IR)
// 4) `a`: 600/816MHz × 모델 × tanh 조합 + A2-Lite·IR 조합을 자동으로 돌리고 tools/benchmark_report.md에 붙일 표를 출력
//
// 시리얼 명령 (115200):
//   0~6 모델 선택 (0 = 없음)   t tanh std/근사   c 600/816MHz   v IR 끔/1024/2048/4096   x IR 정확성 검사
//   i 모델 입력 사인/LINE IN   w LINE OUT L 1kHz 사인/패스스루   k SYS_FS 48k/44.1k (실험)   r 최대값 리셋   a 자동 측정   h 도움말

#include <Arduino.h>
#include <Audio.h>
#include "bench_models.h"
#include "bench_ir.h"

extern "C" uint32_t set_arm_clock(uint32_t frequency);  // cores/teensy4/clockspeed.c (헤더에 선언 없음)

// SGTL5000 enable()은 CHIP_CLK_CTRL에 44.1kHz(0x0004)를 쓰므로 48kHz(0x0008)로 덮어씀.
// write()가 protected라 상속해서 호출합니다.
class Codec48k : public AudioControlSGTL5000 {
public:
  bool setSysFs48k() { return write(0x0004, 0x0008); }  // SYS_FS=48kHz, MCLK=256*Fs
  bool setSysFs44k() { return write(0x0004, 0x0004); }  // enable() 기본값 (실험용)
};

// ---------------------------------------------------------------- 모델
using bench::A2Lite;
using bench::Lstm;
using bench::Model;
using bench::Pool;
using bench::WaveNetA1;

static A2Lite               mA2;
static Lstm<16, 1>          mLstm16("LSTM", "h16, 1층 (A1 Feather LSTM)");
static Lstm<24, 1>          mLstm24("LSTM", "h24, 1층 (A1 Standard LSTM)");
static Lstm<8, 2>           mLstm2x8("LSTM", "h8, 2층 (A1 Lite LSTM)");
static WaveNetA1<4, 2, 2>   mNano("A1 Nano", "ch 4/2, 7+13층");
static WaveNetA1<8, 4, 4>   mFeather("A1 Feather", "ch 8/4, 7+13층");

static Model* const MODELS[] = {nullptr, &mA2, &mLstm16, &mLstm24, &mLstm2x8, &mNano, &mFeather};
constexpr int MODEL_COUNT = sizeof(MODELS) / sizeof(MODELS[0]);

// 링버퍼 풀 (DTCM). 가장 큰 A1 Feather가 약 176KB (PC에서 측정)
constexpr int POOL_FLOATS = 46080;  // 180KB
static float poolMem[POOL_FLOATS] __attribute__((aligned(16)));
static Pool pool(poolMem, POOL_FLOATS);

// IR 저장소는 RAM2(DMAMEM, OCRAM). 펌웨어에서도 A2-Lite 상태(DTCM)와 나눠 둘 예정이라 같은 조건으로 잼
using bench::IrConvolver;
static IrConvolver ir;
DMAMEM static float irSpec[IrConvolver::MAX_PARTS * IrConvolver::N] __attribute__((aligned(32)));
DMAMEM static float irFdl[IrConvolver::MAX_PARTS * IrConvolver::N] __attribute__((aligned(32)));
DMAMEM static float irTmp[IrConvolver::MAX_PARTS * IrConvolver::B] __attribute__((aligned(32)));
DMAMEM static float irTestX[24 * IrConvolver::B], irTestY[24 * IrConvolver::B];  // 정확성 검사용
static const int IR_LENS[] = {0, 1024, 2048, 4096};
constexpr int IR_LEN_COUNT = sizeof(IR_LENS) / sizeof(IR_LENS[0]);

// ---------------------------------------------------------------- 벤치마크 AudioStream
class BenchStream : public AudioStream {
public:
  BenchStream() : AudioStream(1, queue_) {}

  Model* volatile model = nullptr;
  IrConvolver* volatile irConv = nullptr;
  volatile bool fast = false;

  // 통계 (loop()에서 AudioNoInterrupts 안에서 읽고 지움)
  volatile uint32_t cycMax = 0;
  volatile uint64_t cycSum = 0;
  volatile uint32_t irMax = 0;      // IR만
  volatile uint64_t irSum = 0;
  volatile uint32_t blocks = 0;
  volatile float outSq = 0.0f;
  volatile bool outBad = false;

  // 과부하 보호: 모델이 블록 예산의 90%를 3블록 연속 넘으면 오디오 쪽에서 스스로 모델을 뗌.
  // 안 그러면 오디오 인터럽트가 CPU를 다 써서 loop()가 못 돌고 시리얼이 멈춘 것처럼 보임.
  volatile uint32_t budget = 1600000;   // 블록당 사이클, setClock()에서 갱신
  volatile bool overloaded = false;     // selectModel()에서만 지움
  volatile uint32_t overCyc = 0;        // 과부하 때 잰 최대 사이클
  uint8_t overCount = 0;

  void update() override {
    audio_block_t* b = receiveReadOnly(0);
    if (!b) return;
    Model* m = model;
    IrConvolver* c = irConv;
    if (!m && !c) { release(b); return; }

    float in[AUDIO_BLOCK_SAMPLES], out[AUDIO_BLOCK_SAMPLES];
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) in[i] = b->data[i] * (1.0f / 32768.0f);
    release(b);

    const uint32_t t0 = ARM_DWT_CYCCNT;
    if (m) m->process(in, out, AUDIO_BLOCK_SAMPLES, fast);
    else memcpy(out, in, sizeof(out));
    const uint32_t t1 = ARM_DWT_CYCCNT;
    if (c) c->process(out, out);
    const uint32_t t2 = ARM_DWT_CYCCNT;
    const uint32_t dt = t2 - t0;          // 모델 + IR
    const uint32_t dtIr = t2 - t1;
    if (dtIr > irMax) irMax = dtIr;
    irSum += dtIr;

    if (dt > budget / 10 * 9) {
      if (dt > overCyc) overCyc = dt;
      if (++overCount >= 3) { model = nullptr; irConv = nullptr; overloaded = true; overCount = 0; }
    } else {
      overCount = 0;
    }

    if (dt > cycMax) cycMax = dt;
    cycSum += dt;
    blocks++;
    float sq = 0.0f;
    bool bad = false;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      if (!isfinite(out[i])) bad = true;
      sq += out[i] * out[i];
    }
    outSq += sq;
    if (bad) outBad = true;
  }

private:
  audio_block_t* queue_[1];
};

// ---------------------------------------------------------------- 오디오 그래프
AudioSynthWaveformSine sine1k;     // LINE OUT L 주파수 측정용
AudioSynthWaveformSine sineModel;  // 모델 입력용 110Hz
AudioInputI2S          audioIn;
AudioMixer4            srcMix;     // 0: 사인, 1: LINE IN L → 모델
AudioMixer4            outMix;     // 0: 1kHz 사인, 1: LINE IN L → LINE OUT L
BenchStream            benchStream;
AudioOutputI2S         audioOut;
AudioConnection        c1(sineModel, 0, srcMix, 0);
AudioConnection        c2(audioIn, 0, srcMix, 1);
AudioConnection        c3(srcMix, 0, benchStream, 0);
AudioConnection        c4(sine1k, 0, outMix, 0);
AudioConnection        c5(audioIn, 0, outMix, 1);
AudioConnection        c6(outMix, 0, audioOut, 0);
AudioConnection        c7(audioIn, 0, audioOut, 1);  // R은 항상 패스스루
Codec48k               codec;

// ---------------------------------------------------------------- 상태
static int  curModel = 0;
static int  irSel = 0;        // IR_LENS 인덱스
static bool useFastTanh = false;
static bool clk816 = false;
static bool srcLineIn = false;
static bool outSine = true;
static bool sysFs48 = true;

struct Snapshot {
  uint32_t cycMax, blocks, irMax;
  uint64_t cycSum, irSum;
  float outSq;
  bool outBad;
};

static Snapshot takeStats() {
  Snapshot s;
  AudioNoInterrupts();
  s.cycMax = benchStream.cycMax; s.cycSum = benchStream.cycSum; s.blocks = benchStream.blocks;
  s.irMax = benchStream.irMax; s.irSum = benchStream.irSum;
  s.outSq = benchStream.outSq; s.outBad = benchStream.outBad;
  benchStream.cycMax = 0; benchStream.cycSum = 0; benchStream.blocks = 0; benchStream.outSq = 0.0f; benchStream.outBad = false;
  benchStream.irMax = 0; benchStream.irSum = 0;
  AudioInterrupts();
  return s;
}

static void resetMax() {
  takeStats();
  AudioProcessorUsageMaxReset();
  AudioMemoryUsageMaxReset();
}

// 블록 1개에 쓸 수 있는 사이클 (600MHz: 1,600,000)
static float budgetCycles() {
  return (float)F_CPU_ACTUAL * AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT;
}

static void selectModel(int idx) {
  // 오디오가 옛 모델을 안 쓰게 먼저 떼고, 링 할당과 가중치는 loop()에서
  AudioNoInterrupts();
  benchStream.model = nullptr;
  AudioInterrupts();

  curModel = idx;
  Model* m = MODELS[idx];
  if (m) {
    pool.reset();
    if (!m->build(pool)) {
      Serial.printf("!! 풀 부족: %s 에 %d floats 이상 필요 (POOL_FLOATS %d)\n", m->name(), pool.used(), POOL_FLOATS);
      curModel = 0;
      return;
    }
    m->reset();
  }
  AudioNoInterrupts();
  benchStream.overloaded = false;
  benchStream.overCyc = 0;
  benchStream.overCount = 0;
  benchStream.model = m;
  benchStream.irConv = IR_LENS[irSel] ? &ir : nullptr;
  AudioInterrupts();
  resetMax();
}

// IR 길이 선택. 분할 스펙트럼 계산은 여기(loop)에서, 오디오는 그동안 IR 없이 돎
static void selectIr(int sel) {
  AudioNoInterrupts();
  benchStream.irConv = nullptr;
  AudioInterrupts();
  irSel = sel;
  const int len = IR_LENS[sel];
  if (len) {
    bench::makeTestIr(irTmp, len);
    const uint32_t t0 = ARM_DWT_CYCCNT;
    ir.setIr(irTmp, len);
    const uint32_t dt = ARM_DWT_CYCCNT - t0;
    Serial.printf("IR %d샘플 (%.1fms, 분할 %d) 준비: 스펙트럼 계산 %lu us (loop에서)\n", len,
                  len * 1000.0f / AUDIO_SAMPLE_RATE_EXACT, ir.parts(), (unsigned long)(dt / (F_CPU_ACTUAL / 1000000)));
  }
  AudioNoInterrupts();
  benchStream.overloaded = false;
  benchStream.overCyc = 0;
  benchStream.overCount = 0;
  benchStream.irConv = len ? &ir : nullptr;
  if (!benchStream.model && curModel) benchStream.model = MODELS[curModel];  // 과부하로 떨어졌던 모델 복구
  AudioInterrupts();
  resetMax();
}

// IR 정확성: 같은 IrConvolver로 처리한 결과를 직접 컨볼루션과 비교 (CMSIS FFT의 형식·스케일 확인)
static void irSelfTest() {
  AudioNoInterrupts();
  benchStream.irConv = nullptr;
  AudioInterrupts();
  const int len = 2048, blocks = 24;
  bench::makeTestIr(irTmp, len);
  ir.setIr(irTmp, len);
  float* x = irTestX;
  float* y = irTestY;
  uint32_t r = 1;
  for (int i = 0; i < blocks * IrConvolver::B; i++) {
    r = r * 1103515245u + 12345u;
    x[i] = (float)((r >> 8) & 0xffff) / 32768.0f - 1.0f;
  }
  for (int b = 0; b < blocks; b++) ir.process(&x[b * IrConvolver::B], &y[b * IrConvolver::B]);
  float maxErr = 0.0f, maxRef = 0.0f;
  for (int n = 0; n < blocks * IrConvolver::B; n++) {
    float a = 0.0f;
    for (int k = 0; k < len && k <= n; k++) a += irTmp[k] * x[n - k];
    maxErr = fmaxf(maxErr, fabsf(a - y[n]));
    maxRef = fmaxf(maxRef, fabsf(a));
  }
  Serial.printf("IR 정확성 검사 (2048샘플, %d블록): 직접 컨볼루션 대비 최대 오차 %.2e (출력 최대 %.3f) → %s\n",
                blocks, maxErr, maxRef, maxErr < 1e-4f ? "통과" : "실패");
  selectIr(irSel);  // 원래 선택으로 복구
}

static void setClock(bool hi) {
  clk816 = hi;
  set_arm_clock(hi ? 816000000 : 600000000);  // F_CPU_ACTUAL도 같이 바뀌어 CPU% 계산에 반영됨
  benchStream.budget = (uint32_t)budgetCycles();
  resetMax();
}

static const char* modelLabel(int idx) {
  return MODELS[idx] ? MODELS[idx]->name() : "없음";
}

static void printHelp() {
  Serial.println("명령: 0~6 모델 / t tanh / c 클럭 / v IR 길이 / x IR 검사 / i 모델 입력 / w LINE OUT L / k SYS_FS / r 리셋 / a 자동 측정 / h 도움말");
  for (int i = 0; i < MODEL_COUNT; i++) {
    if (!MODELS[i]) { Serial.println("  0  없음 (패스스루만, 기준선)"); continue; }
    Serial.printf("  %d  %-10s %-28s MAC/샘플 %4d, tanh/샘플 %3d\n", i, MODELS[i]->name(), MODELS[i]->config(),
                  MODELS[i]->macsPerSample(), MODELS[i]->tanhPerSample());
  }
}

static void printStatus() {
  Serial.printf("모델 %s, IR %d, tanh %s, %dMHz, 모델 입력 %s, LINE OUT L %s, SYS_FS %s\n",
                modelLabel(curModel), IR_LENS[irSel], useFastTanh ? "근사" : "std", clk816 ? 816 : 600,
                srcLineIn ? "LINE IN" : "110Hz 사인", outSine ? "1kHz 사인" : "패스스루", sysFs48 ? "48k" : "44.1k");
}

// ---------------------------------------------------------------- 자동 측정
struct Result {
  int model;
  bool hi, fast;
  int irLen;
  float maxPct, avgPct, irPct, sysMax;   // maxPct/avgPct는 모델+IR 합, irPct는 IR만 max
  int mem;
  bool bad;
  bool over;   // 과부하로 모델이 떨어짐 (maxPct = 그때 잰 블록 사이클)
};
constexpr int MAX_RESULTS = 32;
static Result results[MAX_RESULTS];
static int resultCount = 0;

constexpr uint32_t SETTLE_MS = 500;
constexpr uint32_t MEASURE_MS = 3000;

static bool sweeping = false;
static int  swStep = 0;          // (클럭, 모델, tanh) 조합 인덱스
static int  swPhase = 0;         // 0 = 설정, 1 = 안정화 대기, 2 = 측정
static elapsedMillis swTimer;

// 조합 인덱스 → 설정. A2-Lite는 tanh를 안 쓰므로 한 번만.
// 마지막에 600MHz A2-Lite + IR 1024/2048/4096 (펌웨어 실제 조합)
static bool stepToConfig(int step, int& model, bool& hi, bool& fast, int& irs) {
  int k = 0;
  irs = 0;
  for (int h = 0; h < 2; h++)
    for (int m = 1; m < MODEL_COUNT; m++)
      for (int t = 0; t < (MODELS[m]->usesTanh() ? 2 : 1); t++) {
        if (k++ == step) { model = m; hi = h; fast = t; return true; }
      }
  for (int i = 1; i < IR_LEN_COUNT; i++)
    if (k++ == step) { model = 1; hi = false; fast = false; irs = i; return true; }
  return false;
}

static const char* verdict(float pct) {
  if (pct <= 35.0f) return "지원 (크로스페이드 가능)";
  if (pct <= 70.0f) return "지원";
  return "불가";
}

static void printTable() {
  Serial.println();
  Serial.println("---- tools/benchmark_report.md 에 붙일 표 ----");
  Serial.println("| 날짜 | 아키텍처 | 설정 | MAC/샘플 | IR | 클럭 | tanh | 블록 | 모델+IR max % | 모델+IR 평균 % | IR max % | 전체 CPU max % | 판정 |");
  Serial.println("|---|---|---|---|---|---|---|---|---|---|---|---|---|");
  for (int i = 0; i < resultCount; i++) {
    const Result& r = results[i];
    Model* m = MODELS[r.model];
    char irLabel[8], irPct[12];
    if (r.irLen) { snprintf(irLabel, sizeof(irLabel), "%d", r.irLen); snprintf(irPct, sizeof(irPct), "%.1f", r.irPct); }
    else { strcpy(irLabel, "-"); strcpy(irPct, "-"); }
    Serial.printf("| | %s | %s | %d | %s | %dMHz | %s | %d | %.1f | %.1f | %s | %.1f | %s%s |\n",
                  m->name(), m->config(), m->macsPerSample(), irLabel, r.hi ? 816 : 600,
                  m->usesTanh() ? (r.fast ? "근사" : "std") : "-", AUDIO_BLOCK_SAMPLES,
                  r.maxPct, r.avgPct, irPct, r.sysMax, r.over ? "불가 (과부하, 모델 max는 떨어지기 전 값)" : verdict(r.sysMax),
                  r.bad ? " (출력 NaN)" : "");
  }
  Serial.println("----------------------------------------------");
}

static void sweepTick() {
  int model; bool hi, fast; int irs;
  if (swPhase == 0) {
    if (!stepToConfig(swStep, model, hi, fast, irs) || resultCount >= MAX_RESULTS) {
      sweeping = false;
      useFastTanh = false;
      benchStream.fast = false;
      setClock(false);
      selectIr(0);
      selectModel(0);
      printTable();
      Serial.println("자동 측정 끝. 600MHz, 모델 없음으로 돌아감");
      return;
    }
    if (hi != clk816) setClock(hi);
    useFastTanh = fast;
    benchStream.fast = fast;
    if (irs != irSel) selectIr(irs);
    selectModel(model);
    Serial.printf("[자동 %d] %s %s, IR %d, %dMHz, tanh %s ...\n", swStep + 1, MODELS[model]->name(), MODELS[model]->config(),
                  IR_LENS[irs], hi ? 816 : 600, MODELS[model]->usesTanh() ? (fast ? "근사" : "std") : "-");
    swTimer = 0;
    swPhase = 1;
  } else if (benchStream.overloaded) {
    // 과부하면 기다리지 않고 바로 기록
    stepToConfig(swStep, model, hi, fast, irs);
    Snapshot s = takeStats();
    Result& r = results[resultCount++];
    r.model = model; r.hi = hi; r.fast = fast; r.irLen = IR_LENS[irs];
    r.maxPct = r.sysMax = benchStream.overCyc * 100.0f / budgetCycles();
    r.avgPct = r.maxPct;
    r.irPct = 0.0f;
    r.mem = AudioMemoryUsageMax();
    r.bad = s.outBad;
    r.over = true;
    Serial.printf("    과부하! 블록당 %.0f%% → 모델을 떼고 다음으로\n", r.maxPct);
    swStep++;
    swPhase = 0;
  } else if (swPhase == 1 && swTimer >= SETTLE_MS) {
    resetMax();  // 모델 교체 직후 블록은 빼고 잼
    swTimer = 0;
    swPhase = 2;
  } else if (swPhase == 2 && swTimer >= MEASURE_MS) {
    stepToConfig(swStep, model, hi, fast, irs);
    Snapshot s = takeStats();
    const float budget = budgetCycles();
    Result& r = results[resultCount++];
    r.model = model; r.hi = hi; r.fast = fast; r.irLen = IR_LENS[irs];
    r.irPct = s.irMax * 100.0f / budget;
    r.maxPct = s.cycMax * 100.0f / budget;
    r.avgPct = s.blocks ? (float)(s.cycSum / s.blocks) * 100.0f / budget : 0.0f;
    r.sysMax = AudioProcessorUsageMax();
    r.mem = AudioMemoryUsageMax();
    r.bad = s.outBad;
    r.over = false;
    Serial.printf("    모델+IR max %.1f%% / 평균 %.1f%% (IR만 max %.1f%%), 전체 CPU max %.1f%%, 온도 %.1fC\n",
                  r.maxPct, r.avgPct, r.irPct, r.sysMax, tempmonGetTemp());
    swStep++;
    swPhase = 0;
  }
}

// ---------------------------------------------------------------- 명령
static void applySources() {
  srcMix.gain(0, srcLineIn ? 0.0f : 1.0f);
  srcMix.gain(1, srcLineIn ? 1.0f : 0.0f);
  outMix.gain(0, outSine ? 1.0f : 0.0f);
  outMix.gain(1, outSine ? 0.0f : 1.0f);
}

static void handleSerial() {
  while (Serial.available()) {
    const char ch = Serial.read();
    if (sweeping) {
      if (ch == 'a') { sweeping = false; Serial.println("자동 측정 중단"); printTable(); }
      continue;  // 자동 측정 중에는 다른 명령 무시
    }
    if (ch >= '0' && ch < '0' + MODEL_COUNT) {
      selectModel(ch - '0');
      printStatus();
    } else switch (ch) {
      case 't': useFastTanh = !useFastTanh; benchStream.fast = useFastTanh; resetMax(); printStatus(); break;
      case 'c': setClock(!clk816); printStatus(); break;
      case 'v': selectIr((irSel + 1) % IR_LEN_COUNT); printStatus(); break;
      case 'x': irSelfTest(); break;
      case 'i': srcLineIn = !srcLineIn; applySources(); resetMax(); printStatus(); break;
      case 'w': outSine = !outSine; applySources(); printStatus(); break;
      case 'k': {
        sysFs48 = !sysFs48;
        const bool ok = sysFs48 ? codec.setSysFs48k() : codec.setSysFs44k();
        Serial.printf("SYS_FS %s 쓰기 %s\n", sysFs48 ? "48k (0x0008)" : "44.1k (0x0004)", ok ? "성공" : "실패");
        break;
      }
      case 'r': resetMax(); Serial.println("최대값 리셋"); break;
      case 'a':
        Serial.println("자동 측정 시작 (25조합, 조합마다 약 3.5초, 다시 a를 누르면 중단)");
        Serial.println("816MHz는 방열판 없이 오래 돌리면 뜨거워집니다. 온도를 같이 봐 주세요.");
        resultCount = 0; swStep = 0; swPhase = 0; sweeping = true;
        break;
      case 'h': printHelp(); printStatus(); break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  AudioMemory(16);

  codec.enable();
  const bool ok = codec.setSysFs48k();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(0);   // 3.12Vpp (hardware-design.md 3-1절)
  codec.lineOutLevel(29);

  sine1k.frequency(1000);
  sine1k.amplitude(0.5f);
  sineModel.frequency(110);
  sineModel.amplitude(0.5f);
  applySources();

  set_arm_clock(600000000);
  benchStream.budget = (uint32_t)budgetCycles();

  if (CrashReport) Serial.print(CrashReport);  // 직전에 하드폴트로 재부팅됐으면 원인 출력
  Serial.println("06_nam_benchmark 시작");
  Serial.printf("AUDIO_SAMPLE_RATE_EXACT = %.1f Hz, 블록 %d 샘플, SYS_FS 48k 설정 %s\n",
                AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES, ok ? "성공" : "실패");
  Serial.printf("CPU %lu MHz, 블록당 예산 %.0f 사이클, 링버퍼 풀 %d KB\n",
                (unsigned long)(F_CPU_ACTUAL / 1000000), budgetCycles(), (int)(POOL_FLOATS * sizeof(float) / 1024));
  ir.attach(irSpec, irFdl);
  irSelfTest();
  printHelp();
  printStatus();
}

void loop() {
  handleSerial();
  if (sweeping) { sweepTick(); return; }

  static elapsedMillis since;
  if (since < 1000) return;
  since = 0;

  static bool overReported = false;
  if (benchStream.overloaded && !overReported) {
    overReported = true;
    Serial.printf("!! 과부하: %s (tanh %s) 블록당 %.0f%% → 모델을 뗐습니다. 다른 번호를 누르거나 t로 근사 tanh를 켜고 다시 고르세요\n",
                  modelLabel(curModel), useFastTanh ? "근사" : "std", benchStream.overCyc * 100.0f / budgetCycles());
  }
  if (!benchStream.overloaded) overReported = false;

  Snapshot s = takeStats();
  const float budget = budgetCycles();
  if ((curModel || IR_LENS[irSel]) && s.blocks) {
    const float maxPct = s.cycMax * 100.0f / budget;
    const float avgPct = (float)(s.cycSum / s.blocks) * 100.0f / budget;
    Serial.printf("[%s | IR %d | %s | %dMHz] 모델+IR max %.1f%% 평균 %.1f%% (IR만 max %.1f%%) | 전체 CPU max %.1f%% | 메모리 max %d | 출력 RMS %.4f%s | %.1fC\n",
                  modelLabel(curModel), IR_LENS[irSel],
                  (curModel && MODELS[curModel]->usesTanh()) ? (useFastTanh ? "근사" : "std") : "-",
                  clk816 ? 816 : 600, maxPct, avgPct, s.irMax * 100.0f / budget, AudioProcessorUsageMax(),
                  AudioMemoryUsageMax(), sqrtf(s.outSq / (s.blocks * AUDIO_BLOCK_SAMPLES)),
                  s.outBad ? " NaN!" : "", tempmonGetTemp());
  } else {
    Serial.printf("[모델 없음 | %dMHz] 전체 CPU %.1f%% (max %.1f%%) | 메모리 max %d | %.1fC\n",
                  clk816 ? 816 : 600, AudioProcessorUsage(), AudioProcessorUsageMax(), AudioMemoryUsageMax(),
                  tempmonGetTemp());
  }
}
