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
                   ├ (교체 중이면 이전/새 모델 크로스페이드)
                   ├ EQ 바이쿼드 3단 → 출력 레벨 → 바이패스 페이드
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
     SYS_FS를 안 바꿨을 때 실제로 무엇이 틀어지는지는 **(확인 필요: SGTL5000 데이터시트의 CHIP_CLK_CTRL 설명)**. 같은 파일의 `autoVolumeControl()`도 44100이 하드코딩돼 있지만 이 프로젝트는 쓰지 않습니다.
  5. **Arduino IDE**: 스케치 안의 `#define`은 라이브러리 `.cpp`에 적용되지 않습니다. 그래서 48kHz가 필요한 `06_nam_benchmark`는 PlatformIO로 빌드합니다. Arduino IDE에서 하려면 Teensyduino의 `platform.txt`/`boards.txt`에 전역 정의를 넣어야 하는데, 지원되는 방법은 **(확인 필요)**.
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
| **A2-Lite** | **1순위 (확정).** A2 파일에서 Lite(3채널) 부분만 추출 |
| LSTM (hidden ≤ 24, 1~2층) | 지원 (예비) |
| A1 WaveNet Nano | 지원 (예비) |
| A1 WaveNet Feather | 벤치마크 후 결정 |
| A1 Lite/Standard, A2-Full | 미지원. 변환기에서 거부 |

### 4-2. 구현 방침
- A2-Lite는 공식 NeuralAmpModelerCore의 A2 구현(임베디드용 활성화 함수 포함)을 기준으로 재구현합니다. Daisy Seed 커뮤니티 구현(블록 48샘플, CPU 약 53%, DTCM 배치)이 참고 사례입니다 (확인 필요: 공식 코어의 A2 소스 위치와 Lite 추출 방법).
- A1/LSTM은 공식 NeuralAmpModelerCore의 `wavenet.cpp`, `lstm.cpp`, `dsp.cpp` 동작을 그대로 따르되 Eigen 대신 고정 크기 배열 + CMSIS-DSP(`arm_dot_prod_f32` 등)로 재구현.
- **가중치 로딩 순서는 공식 코어와 1:1 동일**해야 합니다. 여기가 버그가 가장 많이 나는 곳이라 PC 비교 테스트를 반드시 통과시킵니다.
- WaveNet dilated conv는 레이어별 링버퍼(길이 = dilation × (kernel-1) + 블록)로 구현.
- `tanh`는 빠른 근사(유리함수 또는 테이블+보간)로 교체 후 오차 측정. 오차 1e-4 이하면 채택.
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
| 6 | 2 | 아키텍처 (1=LSTM, 2=A1 WaveNet, 3=A2-Lite) |
| 8 | 4 | 샘플레이트 (48000) |
| 12 | 4 | 설정 블록 길이 N |
| 16 | N | 아키텍처 설정 (레이어 수, 채널, 커널, dilation 목록, head 설정 등, 고정 구조체) |
| 16+N | 4 | 가중치 개수 W |
| 20+N | 4W | float32 리틀엔디언 가중치 (공식 코어 로딩 순서 그대로) |
| 20+N+4W | 가변 | 메타데이터(loudness, 이름 등) TLV |
| 끝-4 | 4 | CRC32 |

PC 쪽 `tools/nam2namb.py`가 만들고, 펌웨어는 헤더 검증(매직, 버전, 아키텍처 지원 여부, 크기 한도, CRC) 실패 시 거부합니다.

### 5-2. 프리셋 JSON (`/presets/NN_name.json`)
```json
{
  "name": "Plexi Crunch",
  "model": "plexi_nano.namb",
  "gain": 0.62, "bass": 0.50, "mid": 0.50, "treble": 0.71,
  "presence": 0.50, "level": 0.70, "gate": 0.30
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
| EQ | Bass: 로우셸프 120Hz, Mid: 피킹 700Hz Q 0.7, Treble: 하이셸프 3kHz, Presence: 하이셸프 5kHz. 각 ±12dB. 계수 계산은 loop()에서, 오디오에는 계수만 전달 |
| 출력 레벨 | -40~+6dB, 출력단 소프트 클리퍼(tanh)로 디지털 클리핑 방지 |
| 바이패스 | 5~10ms 크로스페이드로 디지털 바이패스 |
| (확장) 캐비닛 IR | 균일 분할 FFT 컨볼루션, IR 길이 512~1024. CPU 여유 있을 때만 |

---

## 7. 모델 로딩·전환 (`storage/ModelLoader`)

상태 머신:
```
IDLE → OPENING → READING(청크 4KB씩, loop() 1회당 1청크) → VALIDATING(CRC, 크기)
     → BUILDING(대기 슬롯에 모델 생성) → PREWARMING(무음 2048샘플 돌려 상태 안정화)
     → READY → (오디오가 다음 블록 경계에서 교체, 15ms 크로스페이드) → IDLE
실패 시 어느 단계에서든 → ERROR(기존 모델 유지, 화면 표시) → IDLE
```
- 크로스페이드 동안 두 모델이 동시에 돌아 CPU가 2배 → 크로스페이드 대신 **짧은 페이드아웃 → 교체 → 페이드인**(각 5ms)이 더 안전합니다. 벤치마크로 결정.
- 프리셋만 바뀌고 모델이 같으면 로딩 생략, 파라미터만 교체.
- **(선택) USB MTP**: `MTP_Teensy` 라이브러리로 PC에서 SD에 파일 복사. MTP 쓰기 중 SD 동시 접근 충돌 방지 위해 "USB 모드" 화면에서만 활성화하고, 종료 시 모델 목록 다시 스캔.

---

## 8. UI (`ui/`)

### 8-1. 입력 처리
- 포트: 1kHz 샘플링, 16샘플 이동평균 + 히스테리시스(1%), 0~1 정규화. 노브 커브(선형/로그) 설정 가능.
- **캐치(soft takeover)**: 페이지·프리셋 바뀌면 각 포트를 "잠금" 상태로 두고, 물리 위치가 저장값을 지나는 순간 잠금 해제. 잠금 중에는 화면에 저장값과 노브 위치를 같이 표시.
- 인코더: 핀 인터럽트로 쿼드러처 디코딩(Encoder 라이브러리), 디텐트당 1스텝. 버튼은 짧게/길게(600ms) 구분.
- 풋스위치: 20ms 디바운스, 짧게(<600ms) = 바이패스 토글, 길게 = 다음 프리셋. 길게 판정은 600ms 도달 즉시 실행(발 떼기 기다리지 않음).

### 8-2. 화면·상태 머신
```
HOME(프리셋 보기)
  인코더 회전 → 프리셋 미리보기(이름만 바뀜) → 누름=로드 / 3초 무조작=취소
  인코더 짧게 누름 → 노브 페이지 순환 AMP → EQ → OUT
  인코더 길게 누름 → MENU
MENU
  ├ Save Preset (덮어쓰기 / 새로 저장, 이름은 인코더로 문자 선택)
  ├ Select Model (/models 목록 → 현재 프리셋의 모델 교체)
  ├ Settings (입력/출력 코덱 레벨, 노브 커브, 화면 밝기)
  ├ USB Mode (MTP)
  └ Info (CPU%, 메모리, 펌웨어 버전)
```

HOME 화면 예시 (128x64):
```
[03] Plexi Crunch       BYP
plexi_nano.namb
AMP  Gain  Bass  Treb
     6.2   5.0   7.1
CPU 61%                48k
```
- 화면 라이브러리: U8g2(SH1106/SSD1306 둘 다 지원). 프레임버퍼 방식으로 변경 시에만 전송, 최대 20fps.
- 노브를 돌리는 동안 1.5초간 해당 파라미터 큰 글씨 팝업.

---

## 9. 프로젝트 구조

```
firmware/                      (PlatformIO, board = teensy41)
  platformio.ini
  src/main.cpp
  src/audio/AudioEffectNAM.{h,cpp}
  src/nam/{NamModel.h, Lstm.cpp, WaveNet.cpp, Activations.h, NambFormat.h}
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
| **S0 ★** | 48kHz 패스스루 + 아키텍처별 CPU 벤치마크 | 지원 아키텍처 확정, `benchmark_report.md` |
| **S1 ★** | `nam2namb.py`, `make_reference.py`, PC 빌드 엔진 | PC에서 기준 출력과 최대 오차 1e-4 이하 |
| **S2 ★** | AudioEffectNAM으로 보드에서 모델 재생 | 실제 기타로 모델 톤 확인, CPU% 기록 |
| **S3 ★** | ModelLoader, 프리셋, 상태 저장 | 연속 전환 100회 끊김·크래시 없음 |
| **S4 ★** | 입력 처리, 페이지, 캐치, 풋스위치, 화면 | 모든 조작이 사양대로 동작 |
| S5 | 메뉴(저장/모델 선택/설정), 에러 처리 | SD 제거·손상 파일에서도 소리 유지 |
| S6 | 확장: MTP, 릴레이 바이패스 제어, IR | 시간 남을 때 |

### 테스트 항목
- 단위(PC): 가중치 로딩 순서, LSTM/WaveNet 출력 비교, 바이쿼드 주파수 응답, `.namb` 파서의 손상 파일 거부.
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
- 앰프 모델: Neural Amp Modeler. 공식 코어는 무거우므로 A2-Lite(1순위), LSTM(hidden ≤ 24), A1 WaveNet Nano(Feather는 벤치마크 후)의 순전파만
  Arduino 의존성 없는 순수 C++ + CMSIS-DSP로 재구현한다. 가중치 로딩 순서는 공식 NeuralAmpModelerCore와 1:1 동일해야 한다.
- 모델 파일: PC에서 .nam(JSON) → .namb(자체 바이너리: 매직 "NAMB", 버전, 아키텍처, 샘플레이트, 설정 블록, float32 가중치, 메타데이터 TLV, CRC32) 변환. SD /models/.
- 프리셋: SD /presets/*.json (이름, 모델 파일명, gain/bass/mid/treble/presence/level/gate 0~1). 상태: /system/state.json.
- 신호 체인: 노이즈게이트 → 입력 게인 → NAM → EQ(Bass 로우셸프, Mid 피킹, Treble/Presence 하이셸프) → 출력 레벨 → 소프트 클리퍼. 디지털 바이패스(페이드). FX 루프는 아날로그라 펌웨어와 무관.
- 조작계: B10K 포트 3개(페이지 AMP: Gain/Bass/Treble, EQ: Bass/Mid/Treble, OUT: Level/Gate/Presence, 캐치 방식),
  EC11 인코더(회전=프리셋/값, 짧게=페이지, 길게=메뉴), 풋스위치(짧게=바이패스, 길게 600ms=다음 프리셋), 1.3" OLED 128x64 SPI(U8g2).
- 핀: 포트 14/15/16, 인코더 2/3/4, 풋스위치 5, LED 9, OLED SPI 11/13 + CS 10 + DC 24 + RST 25 (회로 쪽 확정 전 초안).

[규칙]
- 오디오 update() 안에서 SD 접근, 동적 할당, Serial, 화면 갱신 금지. 모든 버퍼는 정적 할당.
- 오디오↔메인 데이터 교환은 블록 경계에서 원자적으로. 모델 교체는 포인터 교체 + 페이드.
- 핫루프는 FASTRUN, 성능 관련 주장은 반드시 측정 코드와 함께 준다.
- 불확실한 라이브러리 동작이나 NAM 내부 구조는 추측하지 말고 "확인 필요"로 표시하고, 공식 소스의 어느 파일을 보면 되는지 알려준다.
- 코드는 모듈 단위로 나눠 주고, 각 단계마다 PC 또는 보드에서 확인할 테스트를 같이 준다.
- 답변은 한국어로.

[지금까지 결정된 것]
- (단계가 끝날 때마다 여기에 추가: 벤치마크 결과, 지원 아키텍처, 핀 확정 등)
```

### S0 실현 가능성 벤치마크
```
S0를 하자.
1. Teensy Audio Library를 48kHz로 동작시키는 방법(AUDIO_SAMPLE_RATE_EXACT, I2S/PLL4 설정, SGTL5000 설정)과 검증 방법(사인파 생성 후 주파수 측정).
2. NAM 공식 코드 기준으로 A2-Lite, LSTM(hidden 16, 24), A1 WaveNet Nano, Feather의 레이어 구성과 샘플당 MAC 수, tanh 호출 수를 계산하고 근거 파일을 밝혀줘.
3. 더미 가중치로 각 아키텍처 순전파를 돌리는 벤치마크 AudioStream 객체. 600MHz/816MHz, std::tanh/근사 tanh 조합별 AudioProcessorUsageMax를 시리얼로 출력.
4. 결과 기록용 benchmark_report.md 양식과 "CPU 70% 이하" 기준의 지원 아키텍처 결정 방법.
```

### S1 변환기 + PC 추론 엔진
```
S1을 하자.
1. .namb 포맷 명세 문서를 먼저 확정(공통 컨텍스트의 구조 기준, 설정 블록 구조체를 A2-Lite/LSTM/A1 WaveNet별로 정의. A2 파일에서 Lite 부분만 추출).
2. tools/nam2namb.py: .nam JSON 파싱, 지원 아키텍처 검증(미지원은 이유와 함께 거부), .namb 저장, 왕복 검증.
3. tools/make_reference.py: 공식 NAM 파이썬 패키지로 테스트 신호(사인 스윕, 임펄스, 기타 DI wav) 처리해 기준 출력 저장.
4. src/nam/: A2-Lite, LSTM, A1 WaveNet 순전파(정적 메모리, 링버퍼 dilated conv, 근사 tanh 옵션). 가중치 순서 근거를 공식 코어 소스 기준으로 주석에.
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
- U8g2 SH1106 SPI, 변경 시에만 최대 20fps 갱신, 노브 조작 시 1.5초 팝업.
- UI는 Params 구조체만 바꾸고 오디오 코드는 직접 건드리지 않게.
```

### S5~S6 마감·확장
```
S5: 에러 처리와 설정 메뉴를 마무리하자(SD 제거, 손상 파일, 프리셋 이름 편집, 코덱 레벨 설정).
S6(선택): MTP_Teensy로 USB 모드(활성화 중 SD 접근 잠금, 종료 시 재스캔), 릴레이 바이패스 제어(전환 전 뮤트), 남은 CPU로 짧은 캐비닛 IR 컨볼루션 가능 여부 측정.
```
