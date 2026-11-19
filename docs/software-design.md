# 소프트웨어 설계 계획 — Teensy 4.1 NAM 앰프 모델러

전체 개요와 일정은 [plan.md](plan.md), 회로는 [hardware-design.md](hardware-design.md)를 보세요.
표시 규칙: **(확정)** = 이대로 진행, **(벤치마크 후 결정)** = Phase S0 결과로 확정.

---

## 1. 설계 원칙

1. **오디오 인터럽트는 신성불가침**: `update()` 안에서는 SD 접근, `malloc/new`, Serial, 화면 갱신 금지. 정해진 시간 안에 끝나는 계산만 합니다.
2. **정적 메모리**: 모델 가중치·상태 버퍼는 부팅 시 최대 크기로 미리 잡아둡니다. 실행 중 동적 할당 없음.
3. **PC와 같은 코드**: 추론 엔진은 Arduino 의존성 없는 순수 C++로 짜서, PC에서도 빌드해 공식 NAM 출력과 비교 테스트합니다.
4. **실패해도 소리는 난다**: 모델 로딩 실패, SD 없음 등 어떤 오류에도 기존 모델(또는 바이패스)로 소리를 유지하고 화면에 에러를 띄웁니다.

---

## 2. 실행 구조

```
[오디오 인터럽트, 128샘플마다 = 2.67ms @48kHz]
  AudioInputI2S → AudioEffectNAM::update() → AudioOutputI2S
                   ├ int16 → float
                   ├ 노이즈게이트 → 입력 게인
                   ├ NAM 추론 (현재 모델 포인터)
                   ├ (교체 중이면 페이드아웃 → 포인터 교체 → 페이드인)
                   ├ EQ 바이쿼드 3단 → 마스터 → 바이패스 페이드
                   └ float → int16 (소프트 클리핑)

[loop(), 메인 컨텍스트]
  Input.poll()     포트·인코더·풋스위치 → 이벤트 큐
  UI.handle(evt)   상태 머신 → 파라미터 변경 요청 / 프리셋 변경 요청
  Params.commit()  변경된 파라미터를 오디오 쪽에 원자적으로 전달
  ModelLoader.step()  SD에서 조금씩 읽기 → 검증 → 준비 완료 시 교체 요청
  Display.refresh() 변경 있을 때만, 최대 20fps
```

- 오디오 ↔ 메인 간 데이터 전달: 파라미터는 `struct Params`를 더블 버퍼로 두고 `volatile` 인덱스 교체(블록 경계에서 반영). 모델 교체는 포인터 1개 교체(32비트 쓰기는 원자적)로 처리.
- 파라미터 스무딩: 게인·레벨·EQ 변경은 블록 내 선형 보간(지퍼 노이즈 방지).

---

## 3. 오디오 설정 (확정 + 확인 필요)

- 샘플레이트 **48kHz**: Teensy Audio Library 기본은 44.1kHz. 대안: 44.1kHz로 두고 약간의 톤 차이 감수.
- 48kHz 설정 방법 (소스 확인함, 보드 검증은 S0):
  1. **상수**: `AUDIO_SAMPLE_RATE_EXACT`는 Teensy 코어 `cores/teensy4/AudioStream.h`에 `#ifndef AUDIO_SAMPLE_RATE_EXACT` 가드로 정의되어 있어 컴파일 옵션 `-D`로 재정의할 수 있습니다.
  2. **I2S 클럭(PLL4)**: Teensy 4.x에서는 코드 수정이 필요 없습니다. Audio 라이브러리 `output_i2s.cpp`의 `AudioOutputI2S::config_i2s()`가 `int fs = AUDIO_SAMPLE_RATE_EXACT;`로 PLL4 계수를 계산해 `set_audioClock()`을 부르고, `AudioInputI2S`도 같은 함수로 클럭을 잡습니다.
  3. **PlatformIO**: `platformio.ini`의 `build_flags`에 `-DAUDIO_SAMPLE_RATE_EXACT=48000.0f`를 넣습니다. `build_flags`는 프로젝트 소스뿐 아니라 프레임워크(코어)와 라이브러리 빌드에도 적용되므로 모든 파일이 같은 값을 봅니다. `build_src_flags`는 `src/`에만 적용되어 라이브러리와 값이 어긋나므로 쓰지 않습니다. 따라서 **수정한 Audio 라이브러리를 `firmware/lib/`에 복사할 필요는 없습니다.**
  4. **SGTL5000**: `AudioControlSGTL5000::enable()`이 `CHIP_CLK_CTRL`(0x0004)에 `0x0004`(SYS_FS = 44.1kHz, MCLK = 256×Fs)를 씁니다(`control_sgtl5000.cpp`). 48kHz는 SYS_FS = 0x2라 `0x0008`입니다. `write()`가 `protected`라 상속 클래스에서 `enable()` 직후 덮어씁니다.
     ```cpp
     class Codec48k : public AudioControlSGTL5000 {
     public:
       bool setSysFs48k() { return write(0x0004, 0x0008); }  // SYS_FS=48kHz, MCLK=256*Fs
     };
     ```
     SYS_FS를 안 바꿨을 때 실제로 무엇이 틀어지는지는 **(확인 필요: SGTL5000 데이터시트의 CHIP_CLK_CTRL 설명. S0에서 확인)**. 같은 파일의 `autoVolumeControl()`도 44100이 하드코딩돼 있지만 이 프로젝트는 쓰지 않습니다.
  5. **소스 안의 `#define`은 쓰지 않음**: 스케치나 `main.cpp` 안의 `#define`은 라이브러리 `.cpp`에 적용되지 않습니다. 이 프로젝트는 테스트 스케치까지 모두 PlatformIO로 빌드하므로 Arduino IDE(Teensyduino) 설정은 다루지 않습니다.
  6. **검증**: 시리얼에 `AUDIO_SAMPLE_RATE_EXACT`를 출력하고, `AudioSynthWaveformSine`으로 1kHz를 내보내 주파수를 잽니다. 1000Hz면 정상, 약 1088Hz면 클럭만 48kHz이고 상수가 44.1kHz, 약 919Hz면 그 반대입니다.
- 블록 크기 128샘플(`AUDIO_BLOCK_SAMPLES` 기본값). CPU가 빠듯하면 함수 호출 오버헤드 때문에 블록이 큰 게 유리하고, 지연은 늘어납니다.
- 코덱: `sgtl5000.inputSelect(AUDIO_INPUT_LINEIN)`, `lineInLevel()`, `lineOutLevel()`로 레벨 맞춤.
- CPU 클럭: 기본 600MHz. 필요 시 `set_arm_clock(816000000)` (방열 확인).
- 처리 정밀도: float32 (M7 단정밀 FPU).

---

## 4. NAM 추론 엔진 (`nam/`)

### 4-1. 지원 범위
| 아키텍처 | 상태 |
|---|---|
| **A2-Lite** | **유일한 지원 대상 (확정).** A2 파일에서 Lite(3채널) 부분만 추출. S0: 600MHz CPU max 49.6% |
| A1 WaveNet (Nano~Standard), LSTM | 미지원 (확정). 변환기에서 거부. S0: 600MHz에서 근사 tanh를 써도 70% 초과 (Nano 84%, LSTM 2×8 77%) |
| A2-Full | 미지원 (확정). 연산량이 A2-Lite의 약 7배 |

근거와 측정값: [tools/benchmark_report.md](../tools/benchmark_report.md). CPU 클럭은 **600MHz (확정)**, 오버클럭 없음.

### 4-2. 구현 방침
- 기준 소스: 공식 NeuralAmpModelerCore `NAM/wavenet/a2_fast.{h,cpp}` (A2 전용 경로, `A2FastModel<3>`이 A2-Lite). 구조가 고정입니다: 23층 = dilation `1,3,7,17,41,101,239` 묶음(커널 6) 3개, 그중 2번째와 3번째 묶음 사이에 커널 15 층 2개(dilation `1`, `13`). 채널 3(= bottleneck), LeakyReLU 0.01, 게이트·FiLM 없음, layer1x1 있음, 헤드 = 커널 16 컨볼루션(3 → 1, bias), 가중치 스트림 마지막 값이 head_scale. 가중치 읽는 순서도 같은 파일 `_load_weights()`에 있습니다.
- A2 `.nam` 파일은 `SlimmableContainer`이고 `submodels`에 Lite(`max_value` 0.5, channels 3)와 Full(1.0, channels 8)이 들어 있습니다(공식 `example_models/A2.nam`). 변환기는 channels 3 서브모델만 꺼냅니다.
- 벤치마크 구현 `sketches/06_nam_benchmark/src/bench_models.h`의 `A2Lite`를 출발점으로 씁니다(PC에서 단순 기준 구현과 최대 오차 6e-9 확인).
- **가중치 로딩 순서는 공식 코어와 1:1 동일**해야 합니다. 여기가 버그가 가장 많이 나는 곳이라 PC 비교 테스트를 반드시 통과시킵니다.
- dilated conv는 레이어별 링버퍼(길이 = dilation × (kernel-1) + 블록, 뒤에 블록 길이 미러)로 구현. 상태 메모리 약 147KB.
- tanh는 쓰지 않습니다(LeakyReLU). LeakyReLU는 `fmaxf(a, 0.01f * a)`로 분기 없이 계산(VMAXNM).
- 핫루프에서 포인터는 `__restrict`, 입력은 지역 변수로 한 번만 읽고 출력 채널을 번갈아 누적합니다. S0에서 이것만으로 76.5% → 49.1%가 됐습니다(benchmark_report.md 2절).
- 핫루프 함수는 `FASTRUN`(ITCM), 가중치·상태는 `DMAMEM` 대신 DTCM 우선 배치(크기가 작아서 가능).
- 모델 메타데이터의 `loudness`, `input_level_dbu`, `output_level_dbu`가 있으면 입출력 레벨 정규화에 사용.

### 4-3. 인터페이스
```cpp
class NamModel {
public:
  virtual void reset() = 0;                       // 상태 버퍼 0으로
  virtual void process(const float* in, float* out, int n) = 0;
  virtual void prewarm(int samples) = 0;          // 로딩 직후 무음으로 상태 안정화
};
// 정적 풀에서 생성
NamModel* createModel(const NambHeader& h, const float* weights, ModelSlot& slot);
```
모델 슬롯 2개(현재/대기)를 정적으로 잡아 두고 교체 시 번갈아 씁니다.

---

## 5. 파일 형식

### 5-1. `.namb` (자체 바이너리, 확정)
| 오프셋 | 크기 | 내용 |
|---|---|---|
| 0 | 4 | 매직 `"NAMB"` |
| 4 | 2 | 포맷 버전 (1) |
| 6 | 2 | 아키텍처 (3=A2-Lite만 지원. 1, 2는 예약) |
| 8 | 4 | 샘플레이트 (48000) |
| 12 | 4 | 설정 블록 길이 N (**4의 배수**) |
| 16 | N | 아키텍처 설정 (레이어 수, 채널, 커널, dilation 목록, head 설정 등, 고정 구조체). 4의 배수가 되도록 끝을 0으로 채움 |
| 16+N | 4 | 가중치 개수 W |
| 20+N | 4W | float32 리틀엔디언 가중치 (공식 코어 로딩 순서 그대로) |
| 20+N+4W | 가변 | 메타데이터(loudness, 이름 등) TLV |
| 끝-4 | 4 | CRC32 |

PC 쪽 `tools/nam2namb.py`가 만들고, 펌웨어는 헤더 검증(매직, 버전, 아키텍처 지원 여부, 크기 한도, **N % 4 == 0**, CRC) 실패 시 거부합니다.

**정렬 규칙**: 가중치 시작 오프셋 20+N이 4의 배수여야 파일 버퍼를 복사 없이 `const float*`로 캐스팅할 수 있습니다(20이 4의 배수이므로 N이 4의 배수면 됨).
- Cortex-M7은 일반 정수 로드는 비정렬을 허용하지만 FPU 로드(`VLDR`)와 `LDRD`/`LDM`은 비정렬이면 UsageFault가 납니다. 컴파일러는 `float*`가 4바이트 정렬이라고 가정하고 이 명령을 씁니다.
- 변환기는 설정 구조체 뒤를 0으로 채워 N을 4의 배수로 만들고, 펌웨어 파서는 N % 4 != 0이면 거부합니다.
- 파일을 읽어 넣는 버퍼도 `alignas(4)` 이상(권장 `alignas(32)`, 캐시 라인)으로 선언합니다.

### 5-2. 프리셋 JSON (`/presets/NN_name.json`)
```json
{
  "name": "Plexi Crunch",
  "model": "plexi_crunch.namb",
  "gain": 0.62, "gate": 0.30, "master": 0.80,
  "bass": 0.50, "mid": 0.45, "treble": 0.71
}
```
- 노브 값은 0~1로 저장하고 실제 dB/주파수 매핑은 펌웨어에서.
- `/system/state.json`에 마지막 프리셋 번호, 바이패스 상태 저장(부팅 시 복원). 쓰기는 변경 후 2초 지연(디바운스)으로 SD 수명 보호.

### 5-3. SD 폴더 구조
```
/models/    *.namb
/presets/   01_clean.json ...
/system/    state.json, settings.json(코덱 레벨, 노브 커브 등)
```

---

## 6. DSP 전후처리 (`dsp/`)

| 블록 | 설계 |
|---|---|
| 노이즈 게이트 | 엔벨로프 팔로워(어택 1ms, 릴리즈 50ms) + 히스테리시스, 임계값 -80~-40dBFS |
| 입력 게인 | -12~+24dB, 모델 앞 |
| EQ | 3밴드. Bass: 로우셸프 120Hz, Mid: 피킹 700Hz Q 0.7, Treble: 하이셸프 3kHz. 각 ±12dB. 계수 계산은 loop()에서, 오디오에는 계수만 전달 |
| 마스터(출력 레벨) | -40~+6dB, 출력단 소프트 클리퍼(tanh)로 디지털 클리핑 방지 |
| 바이패스 | 5~10ms 크로스페이드로 디지털 바이패스 |
| (확장) 캐비닛 IR | 균일 분할 FFT 컨볼루션(분할 = 블록 128, 추가 지연 없음), IR 길이 1024~2048. S0 결과 A2-Lite 49.6%라 여유 있음. IR 비용은 06에서 측정 후 확정 |

---

## 7. 모델 로딩·전환 (`storage/ModelLoader`)

상태 머신:
```
IDLE → OPENING → READING(청크 4KB씩, loop() 1회당 1청크) → VALIDATING(CRC, 크기)
     → BUILDING(대기 슬롯에 모델 생성) → PREWARMING(무음 2048샘플 돌려 상태 안정화)
     → READY → (오디오가 5ms 페이드아웃 → 블록 경계에서 포인터 교체 → 5ms 페이드인) → IDLE
실패 시 어느 단계에서든 → ERROR(기존 모델 유지, 화면 표시) → IDLE
```
- **교체 방식 (확정): 5ms 페이드아웃 → 교체 → 5ms 페이드인.** 교체 중 약 10ms 동안 소리가 작아지지만 모델은 항상 1개만 돕니다.
- 크로스페이드(10~20ms)는 그동안 두 모델이 동시에 돌아 CPU가 2배가 됩니다. 모델 1개의 CPU 최대치가 35% 이하일 때만 가능한데, **S0 결과 A2-Lite가 49.6%라 쓰지 않습니다 (확정).**
- 프리셋만 바뀌고 모델이 같으면 로딩 생략, 파라미터만 교체.
- **(선택) USB MTP**: `MTP_Teensy` 라이브러리로 PC에서 SD에 파일 복사. MTP 쓰기 중 SD 동시 접근 충돌 방지 위해 "USB 모드" 화면에서만 활성화하고, 종료 시 모델 목록 다시 스캔.

---

## 8. UI (`ui/`)

### 8-1. 입력 처리
- 포트: 1kHz 샘플링, 16샘플 이동평균 + 히스테리시스(1%), 0~1 정규화. 노브 커브(선형/로그) 설정 가능.
- **캐치(soft takeover)**: 페이지·프리셋 바뀌면 각 포트를 "잠금" 상태로 두고, 물리 위치가 저장값을 지나는 순간 잠금 해제. 잠금 중에는 화면에 저장값과 노브 위치를 같이 표시.
- 인코더: 핀 인터럽트로 쿼드러처 디코딩(Encoder 라이브러리), 디텐트당 1스텝. 버튼은 짧게/길게(600ms) 구분.
- 풋스위치: 20ms 디바운스, 짧게(<600ms) = 바이패스 토글, 길게 = 다음 프리셋. 길게 판정은 600ms 도달 즉시 실행(발 떼기 기다리지 않음).
- 풋스위치로 **이전 프리셋으로 가는 동작은 없습니다(의도).** 스위치가 1개라 동작을 늘리면 오조작이 생기므로, 이전 프리셋은 인코더 회전으로 고릅니다. 마지막 프리셋에서 길게 누르면 첫 프리셋으로 돌아갑니다.

### 8-2. 노브 페이지 (확정)
| 페이지 | 포트 1 | 포트 2 | 포트 3 |
|---|---|---|---|
| EQ | Bass (`bass`) | Mid (`mid`) | Treble (`treble`) |
| IN/OUT | Input Gain (`gain`) | Noise Gate (`gate`) | Master (`master`) |

- 괄호는 프리셋 JSON 필드입니다. 파라미터는 이 6개가 전부이고, 페이지마다 겹치는 파라미터는 없습니다.
- 페이지를 바꾸면 캐치가 걸리므로, 포트 위치가 새 페이지의 저장값을 지나기 전까지는 값이 바뀌지 않습니다.
- 화면 라벨은 칸 폭(30px, 6x10 글꼴 5자) 때문에 Bass/Mid/Treb, Gain/Gate/Mstr로 줄여 씁니다.

### 8-3. 화면·상태 머신
```
HOME(프리셋 보기)
  인코더 회전 → 프리셋 미리보기(이름만 바뀜) → 누름=로드 / 3초 무조작=취소
  인코더 짧게 누름 → 노브 페이지 전환 EQ ↔ IN/OUT
  인코더 길게 누름 → MENU
MENU
  ├ Save Preset (덮어쓰기 / 새로 저장, 이름은 인코더로 문자 선택)
  ├ Select Model (/models 목록 → 현재 프리셋의 모델 교체)
  ├ Settings (입력/출력 코덱 레벨, 노브 커브, 화면 밝기)
  ├ USB Mode (MTP)
  └ Info (CPU%, 메모리, 샘플레이트, 펌웨어 버전)
```

HOME 화면 예시 (128x32, 6x10 글꼴 3줄):
```
[03] Plexi Crunch     BYP
Bass  Mid  Treb   I [####  ]
 5.0  4.5   7.1   O [###   ]
```
- 1줄: 프리셋 번호와 이름(왼쪽), 바이패스 표시(오른쪽 끝).
- 2~3줄 왼쪽(x 0~89): 현재 노브 페이지의 라벨과 값, 30px 3칸 가운데 정렬. 페이지가 바뀌어도 같은 포트는 같은 칸.
- 2~3줄 오른쪽(x 92~127): 입력(I)/출력(O) 레벨 미터, 가로 막대. 오디오 쪽이 블록마다 피크값을 넘기고 UI가 dBFS(-48~0)를 0~1로 바꿔 그림.
- 모델 파일명은 HOME에 없음(Select Model에서 확인). CPU%와 샘플레이트는 MENU > Info에서.
- 배치와 좌표는 `sketches/04_oled_display`에 있음. 5x7 글꼴 4줄(모델 파일명 줄 추가) 배치도 같은 스케치에서 비교 가능.
- 화면: Waveshare 2.23" OLED 128x32 SPI (SSD1305). 라이브러리 U8g2, 생성자 `U8G2_SSD1305_128X32_ADAFRUIT_F_4W_HW_SPI`(Waveshare는 Adafruit와 같은 설정). 프레임버퍼 방식으로 변경 시에만 전송, 최대 20fps. 미터가 움직이는 동안은 매 프레임 바뀜.
- 노브를 돌리는 동안 1.5초간 해당 파라미터 큰 글씨 팝업.

---

## 9. 프로젝트 구조

```
firmware/                      (PlatformIO, board = teensy41)
  platformio.ini
  src/main.cpp
  src/audio/AudioEffectNAM.{h,cpp}
  src/nam/{NamModel.h, A2Lite.{h,cpp}, NambFormat.h}
  src/dsp/{Gate, Biquad, Gain, SoftClip}.{h,cpp}
  src/storage/{ModelLoader, Presets, SystemState}.{h,cpp}
  src/ui/{Inputs, Pages, Menu, Display}.{h,cpp}
  src/Params.h                 오디오↔UI 공유 파라미터
test_pc/                       PC용 CMake, nam/ 와 dsp/ 를 그대로 빌드
  test_reference.cpp           기준 출력과 비교
tools/
  nam2namb.py                  변환 + 아키텍처 검증
  make_reference.py            공식 NAM으로 기준 입출력 wav 생성
  benchmark_report.md          S0 결과 기록
```

라이브러리: Teensy Audio, SD(SdFat, Teensy 내장), ArduinoJson, U8g2, Encoder, CMSIS-DSP(Teensyduino 포함), (선택) MTP_Teensy.

---

## 10. 개발 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| **S0 ★** | 48kHz 패스스루 + 아키텍처별 CPU 벤치마크 | 지원 아키텍처 확정, `benchmark_report.md` (**CPU 측정 완료: A2-Lite만**, 48kHz 주파수 검증 남음) |
| **S1 ★** | `nam2namb.py`, `make_reference.py`, PC 빌드 엔진 | PC에서 기준 출력과 최대 오차 1e-4 이하 |
| **S2 ★** | AudioEffectNAM으로 보드에서 모델 재생 | 실제 기타로 모델 톤 확인, CPU% 기록 |
| **S3 ★** | ModelLoader, 프리셋, 상태 저장 | 연속 전환 100회 끊김·크래시 없음 |
| **S4 ★** | 입력 처리, 페이지, 캐치, 풋스위치, 화면 | 모든 조작이 사양대로 동작 |
| S5 | 메뉴(저장/모델 선택/설정), 에러 처리 | SD 제거·손상 파일에서도 소리 유지 |
| S6 | 확장: MTP, 릴레이 바이패스 제어, IR | 시간 남을 때 |

### 테스트 항목
- 단위(PC): 가중치 로딩 순서, A2-Lite 출력 비교, 바이쿼드 주파수 응답, `.namb` 파서의 손상 파일 거부.
- 보드: CPU 최대치(`AudioProcessorUsageMax`), 오디오 메모리 사용(`AudioMemoryUsageMax`), 모델 전환 중 클릭 유무, 1시간 연속 동작.

---

## 11. 소프트웨어 설계용 프롬프트

새 Claude 세션(Claude Code 권장)에 아래 공통 컨텍스트를 먼저 붙이고, 단계별 요청을 이어서 붙이세요.

### 공통 컨텍스트
```
너는 임베디드 오디오 DSP 펌웨어 엔지니어로서 나와 함께 Teensy 4.1 기반 NAM 앰프 모델러 페달의 펌웨어를 만든다.

[프로젝트]
- Teensy 4.1 (Cortex-M7 600MHz, 단정밀 FPU, RAM 1MB), PJRC Audio Shield Rev D(SGTL5000), Teensy Audio Library, PlatformIO.
- 샘플레이트 48kHz, 블록 128샘플, float32 처리.
- 앰프 모델: Neural Amp Modeler. 공식 코어는 무거우므로 A2-Lite의 순전파만
  Arduino 의존성 없는 순수 C++ + CMSIS-DSP로 재구현한다. 가중치 로딩 순서는 공식 NeuralAmpModelerCore와 1:1 동일해야 한다.
- 모델 파일: PC에서 .nam(JSON) → .namb(자체 바이너리: 매직 "NAMB", 버전, 아키텍처, 샘플레이트, 설정 블록, float32 가중치, 메타데이터 TLV, CRC32) 변환. SD /models/.
- 프리셋: SD /presets/*.json (이름, 모델 파일명, gain/gate/master/bass/mid/treble 0~1). 상태: /system/state.json.
- 신호 체인: 노이즈게이트 → 입력 게인 → NAM → EQ 3밴드(Bass 로우셸프, Mid 피킹, Treble 하이셸프) → 마스터 → 소프트 클리퍼. 디지털 바이패스(페이드). FX 루프는 아날로그라 펌웨어와 무관.
- 조작계: B10K 포트 3개(페이지 EQ: Bass/Mid/Treble, IN/OUT: Input Gain/Noise Gate/Master, 캐치 방식),
  EC11 인코더(회전=프리셋/값, 짧게=페이지, 길게=메뉴), 풋스위치(짧게=바이패스, 길게 600ms=다음 프리셋), Waveshare 2.23" OLED 128x32 SPI(SSD1305, U8g2), HOME에 입력/출력 레벨 미터.
- 핀: 포트 14/15/16, 인코더 2/3/4, 풋스위치 5, LED 9, OLED SPI 11/13 + CS 10 + DC 24 + RST 25 (회로 쪽 확정 전 초안).

[규칙]
- 오디오 update() 안에서 SD 접근, 동적 할당, Serial, 화면 갱신 금지. 모든 버퍼는 정적 할당.
- 오디오↔메인 데이터 교환은 블록 경계에서 원자적으로. 모델 교체는 5ms 페이드아웃 → 포인터 교체 → 5ms 페이드인.
- 핫루프는 FASTRUN, 성능 관련 주장은 반드시 측정 코드와 함께 준다.
- 불확실한 라이브러리 동작이나 NAM 내부 구조는 추측하지 말고 "확인 필요"로 표시하고, 공식 소스의 어느 파일을 보면 되는지 알려준다.
- 코드는 모듈 단위로 나눠 주고, 각 단계마다 PC 또는 보드에서 확인할 테스트를 같이 준다.
- 답변은 한국어로.

[지금까지 결정된 것]
- 48kHz는 build_flags -DAUDIO_SAMPLE_RATE_EXACT=48000.0f + SGTL5000 CHIP_CLK_CTRL=0x0008(Codec48k). 보드 검증은 S0, 06은 독립 PlatformIO 프로젝트 (software-design.md 3장)
- 모델 교체는 5ms 페이드아웃 → 포인터 교체 → 5ms 페이드인. 크로스페이드는 모델 1개 CPU 35% 이하일 때만 (software-design.md 7장)
- 노브 페이지 EQ: Bass/Mid/Treble, IN/OUT: Input Gain/Noise Gate/Master(`master`), 인코더 짧게로 전환. Presence 없음, 파라미터 6개 (software-design.md 8-2절)
- 화면 Waveshare 2.23" SSD1305 128x32, HOME은 6x10 3줄(프리셋 / 노브 라벨·값 + I/O 미터) (software-design.md 8-3절)
- .namb 설정 블록 길이 N은 4의 배수, 파서는 N % 4 != 0이면 거부, 읽기 버퍼는 alignas(4) 이상 (software-design.md 5-1절)
- 풋스위치 짧게 = 바이패스, 길게 600ms = 다음 프리셋(마지막 다음은 첫 프리셋). 이전 프리셋은 인코더로 (software-design.md 8-1절)
- S0: 지원 아키텍처는 A2-Lite만, 600MHz에서 CPU max 49.6%(기준 70%). A1·LSTM·A2-Full 미지원. 클럭 600MHz, 크로스페이드 없음. 기준 소스는 NeuralAmpModelerCore NAM/wavenet/a2_fast.cpp, 출발 코드는 sketches/06_nam_benchmark/src/bench_models.h (tools/benchmark_report.md)
- (단계가 끝날 때마다 여기에 추가: 48kHz 주파수 검증, 핀 확정 등)
```

### S0 실현 가능성 벤치마크 (CPU 측정 완료, 결과는 tools/benchmark_report.md)
```
S0를 하자.
1. software-design.md 3장에 정리한 48kHz 방법(build_flags -DAUDIO_SAMPLE_RATE_EXACT=48000.0f, Codec48k로 SYS_FS 덮어쓰기)을
   sketches/06_nam_benchmark(독립 PlatformIO 프로젝트) 구조로 보드에서 검증하고, 1kHz 사인 주파수 측정값을
   tools/benchmark_report.md의 "48kHz 설정 검증"에 기록. 3장의 (확인 필요)도 같이 확인:
   - SYS_FS를 44.1kHz로 둔 채(setSysFs48k() 호출 생략) 48kHz 클럭을 넣었을 때 달라지는 점(주파수, 잡음, 왜곡). SGTL5000 데이터시트 CHIP_CLK_CTRL 설명 근거 포함
2. NAM 공식 코드 기준으로 A2-Lite, LSTM(hidden 16, 24), A1 WaveNet Nano, Feather의 레이어 구성과 샘플당 MAC 수, tanh 호출 수를 계산하고 근거 파일을 밝혀줘.
3. 더미 가중치로 각 아키텍처 순전파를 돌리는 벤치마크 AudioStream 객체. 600MHz/816MHz, std::tanh/근사 tanh 조합별 AudioProcessorUsageMax를 시리얼로 출력.
4. 결과 기록용 benchmark_report.md 양식과 "CPU 70% 이하" 기준의 지원 아키텍처 결정 방법.
```

### S1 변환기 + PC 추론 엔진
```
S1을 하자.
1. .namb 포맷 명세 문서를 먼저 확정(공통 컨텍스트의 구조 기준, A2-Lite 설정 블록 구조체 정의. A2 파일(SlimmableContainer)에서 channels 3 서브모델만 추출).
2. tools/nam2namb.py: .nam JSON 파싱, 지원 아키텍처 검증(미지원은 이유와 함께 거부), .namb 저장, 왕복 검증.
3. tools/make_reference.py: 공식 NAM 파이썬 패키지로 테스트 신호(사인 스윕, 임펄스, 기타 DI wav) 처리해 기준 출력 저장.
4. src/nam/: A2-Lite 순전파(정적 메모리, 링버퍼 dilated conv). sketches/06_nam_benchmark/src/bench_models.h의 A2Lite에서 출발. 가중치 순서 근거를 공식 코어 a2_fast.cpp _load_weights() 기준으로 주석에.
5. test_pc/: CMake로 같은 소스를 빌드해 기준 출력과 최대 오차·RMS 오차 출력. 목표 최대 오차 1e-4.
```

### S2 보드에서 모델 재생
```
S2: AudioEffectNAM(AudioStream 상속)을 만들어 보드에서 모델을 돌리자.
- int16↔float 변환, 입력 게인, 메타데이터 loudness 기반 레벨 정규화, 출력 소프트 클리퍼.
- 우선 SD에서 고정 파일 하나를 setup()에서 로드.
- CPU%, 오디오 메모리 사용량을 1초마다 시리얼 출력.
- 실제 기타로 확인할 체크리스트(톤이 PC 플러그인과 비슷한지 비교하는 방법 포함).
```

### S3 로딩·프리셋
```
S3: ModelLoader와 프리셋 시스템.
- 상태 머신(IDLE→OPENING→READING 4KB 청크→VALIDATING→BUILDING→PREWARMING→READY→교체, 실패 시 ERROR)을 loop()에서 비차단으로.
- 모델 슬롯 2개 정적 할당, 블록 경계 포인터 교체 + 5ms 페이드아웃/인. 같은 모델이면 로딩 생략.
- ArduinoJson으로 /presets 읽기/쓰기, /system/state.json 지연 저장(2초 디바운스).
- 테스트: 100회 연속 전환, 손상 파일, SD 미삽입 부팅.
```

### S4 UI
```
S4: UI를 만들자.
- 상태 머신(HOME, 프리셋 미리보기, 페이지 순환, MENU: Save/Select Model/Settings/USB Mode/Info)을 텍스트 다이어그램으로 먼저 보여주고 코드로.
- 포트: 이동평균+히스테리시스, 캐치 방식, 잠금 중 화면 표시. 인코더: Encoder 라이브러리, 짧게/길게 구분. 풋스위치: 디바운스, 600ms 도달 즉시 길게 판정.
- U8g2 SSD1305 128x32 SPI(Waveshare 2.23"), 변경 시에만 최대 20fps 갱신, HOME에 I/O 레벨 미터, 노브 조작 시 1.5초 팝업.
- UI는 Params 구조체만 바꾸고 오디오 코드는 직접 건드리지 않게.
```

### S5~S6 마감·확장
```
S5: 에러 처리와 설정 메뉴를 마무리하자(SD 제거, 손상 파일, 프리셋 이름 편집, 코덱 레벨 설정).
S6(선택): MTP_Teensy로 USB 모드(활성화 중 SD 접근 잠금, 종료 시 재스캔), 릴레이 바이패스 제어(전환 전 뮤트), 남은 CPU로 짧은 캐비닛 IR 컨볼루션 가능 여부 측정.
```
