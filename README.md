# teensy-nam-pedal

Teensy 4.1과 [NAM(Neural Amp Modeler)](https://www.neuralampmodeler.com/)으로 만드는 자작 기타 앰프 모델러 페달입니다.

> 목표: 입대 전에 실제로 페달보드에 올려 쓸 수 있는 앰프 모델러 1대 완성.
> 원칙: 가장 큰 리스크(Teensy에서 NAM이 실시간으로 도는가)를 첫 주에 검증하고, 나머지는 검증된 범위 안에서 만든다.

## 프로젝트 개요

| 항목 | 내용 |
|---|---|
| MCU | Teensy 4.1 (Cortex-M7 600MHz, RAM 1MB) |
| 코덱 | PJRC Audio Shield Rev D (SGTL5000), 48kHz |
| 모델 | 1순위 A2-Lite, 예비 A1 Nano / 소형 LSTM (A1 Standard, A2-Full 미지원) |
| 조작계 | 작은 노브 3개(B10K), 큰 노브 1개(EC11 인코더), 풋스위치 1개, 1.3" OLED 128x64 |
| 입출력 | 1/4" IN, OUT, FX 루프 SEND/RETURN (아날로그, 앰프 모델 뒤) |
| 전원 | 9V DC 센터 마이너스, 500mA 이상 권장 |
| 케이스 | Hammond 1590XX |

신호 흐름:

```
IN → 입력 버퍼(1MΩ) → ADC → 노이즈게이트 → Input Gain → NAM 모델 → EQ → Output Level
   → DAC → 출력 버퍼 → SEND → RETURN(미삽입 시 자동 연결) → 출력 버퍼 → OUT
```

자세한 설계는 [docs/](docs/)에 있습니다.

- [docs/plan.md](docs/plan.md): 전체 계획, 기능 정의, 일정, 리스크
- [docs/hardware-design.md](docs/hardware-design.md): 회로, 핀 배치, BOM, 케이스
- [docs/software-design.md](docs/software-design.md): 펌웨어 구조, `.namb` 포맷, UI

## 폴더 구조

```
docs/          설계 문서 (계획, 회로, 소프트웨어)
hardware/      회로 담당: 회로도, 만능기판 레이아웃, 측정 기록, 사진
sketches/      단계별 테스트 스케치 (Arduino IDE + Teensyduino로 하나씩 업로드)
firmware/      메인 NAM 펌웨어 (PlatformIO)
tools/         PC용 파이썬 도구 (.nam → .namb 변환, 기준 출력 생성)
sd-card/       microSD에 그대로 복사할 폴더 구조와 예시 프리셋
```

## 만드는 순서

하드웨어(H)와 소프트웨어(S)를 같이 진행하되, 아래 테스트 스케치를 순서대로 통과시키면서 부품을 하나씩 붙입니다. 앞 단계가 안 되면 다음 단계로 넘어가지 않습니다.

| 순서 | 스케치 / 작업 | 확인할 것 | 관련 단계 |
|---|---|---|---|
| 1 | [`sketches/01_blink`](sketches/01_blink) | Teensy 업로드 환경이 동작하고 LED가 깜빡임 | H0 |
| 2 | [`sketches/02_audio_passthrough`](sketches/02_audio_passthrough) | 오디오실드 납땜 확인, LINE IN → LINE OUT 소리 통과 | H0, S0 |
| 3 | [`sketches/03_knobs_switches`](sketches/03_knobs_switches) | 포트 3개, 인코더, 풋스위치, LED 모두 인식 | H2 |
| 4 | [`sketches/04_oled_display`](sketches/04_oled_display) | OLED 배선과 화면 표시 | H2 |
| 5 | [`sketches/05_sd_card`](sketches/05_sd_card) | 내장 microSD 읽기/쓰기, `/models` 목록 | S3 준비 |
| 6 | [`sketches/06_nam_benchmark`](sketches/06_nam_benchmark) | 48kHz 동작 + 모델별 CPU 사용률 측정, 지원 범위 결정 | S0 |
| 7 | 브레드보드 아날로그 회로 | 전원부 → Vref → 입력/출력 버퍼, 기타 → 앰프 소리 깨끗 | H1 |
| 8 | [`tools/`](tools) + [`firmware/test_pc`](firmware/test_pc) | `.namb` 변환, PC에서 기준 출력과 오차 1e-4 이하 | S1 |
| 9 | [`firmware/`](firmware) | 보드에서 모델 재생 → 로딩·프리셋 → UI 순서로 기능 추가 | S2~S5 |
| 10 | 만능기판 이식, 케이스 가공 | 브레드보드와 같은 동작, 점검 체크리스트 통과 | H3, H4 |

주 단위 일정과 MVP 범위는 [docs/plan.md의 7장](docs/plan.md#7-단계별-일정-주-단위-약-8주-기준)을 보세요.

## 업로드 방법

### 방법 1: Arduino IDE + Teensyduino (테스트 스케치용)

1. [Arduino IDE 2](https://www.arduino.cc/en/software)를 설치합니다.
2. **File > Preferences > Additional boards manager URLs**에 아래 주소를 넣습니다.
   ```
   https://www.pjrc.com/teensy/package_teensy_index.json
   ```
3. **Boards Manager**에서 `Teensy`를 검색해 설치합니다. Teensy Audio, SD, Encoder 라이브러리가 같이 들어옵니다.
4. OLED 스케치용으로 **Library Manager**에서 `U8g2`를 설치합니다.
5. `sketches/01_blink/01_blink.ino`를 엽니다. 폴더 이름과 `.ino` 이름이 같아야 열립니다.
6. **Tools** 메뉴에서 설정합니다.
   - Board: `Teensy 4.1`
   - USB Type: `Serial` (오디오 스케치도 Serial로 충분)
   - CPU Speed: `600 MHz`
   - Port: Teensy가 연결된 포트
7. **Upload**를 누르면 Teensy Loader가 뜨고 자동으로 올라갑니다. 안 올라가면 Teensy 보드의 흰색 PROGRAM 버튼을 한 번 누르세요.
8. **Tools > Serial Monitor**로 출력을 확인합니다.

### 방법 2: PlatformIO (메인 펌웨어용)

1. VS Code에 **PlatformIO IDE** 확장을 설치합니다.
2. VS Code에서 `firmware/` 폴더를 엽니다 (저장소 루트가 아니라 `firmware/`).
3. 하단 상태바의 체크(빌드), 화살표(업로드), 플러그(시리얼 모니터) 아이콘을 씁니다. 터미널에서는 아래와 같습니다.
   ```bash
   cd firmware
   pio run                 # 빌드
   pio run -t upload       # 업로드
   pio device monitor      # 시리얼 모니터
   ```
4. 필요한 라이브러리는 `platformio.ini`의 `lib_deps`에 적혀 있어서 첫 빌드 때 자동으로 받습니다.

### 주의 사항

- **Linux**는 udev 규칙이 있어야 업로드됩니다: [00-teensy.rules](https://www.pjrc.com/teensy/00-teensy.rules)를 `/etc/udev/rules.d/`에 넣으세요.
- **Teensy 뒷면 VUSB–VIN 패드를 끊은 뒤에는** USB만으로는 전원이 안 들어갑니다. 업로드할 때도 9V 어댑터를 꽂아야 합니다. 패드는 02~06 테스트가 끝나고 전원부를 붙일 때 끊습니다.
- Teensy 4.1 핀은 **3.3V 전용**(5V 톨러런트 아님)입니다. 포트와 스위치는 3.3V 기준으로 배선하세요.

## 협업 규칙

두 명이 회로와 코드를 나눠 맡습니다.

| 역할 | 주로 맡는 폴더 | 내용 |
|---|---|---|
| 하드웨어 | `hardware/`, `docs/hardware-design.md` | 전원부, 버퍼, 조작계 배선, 조립, 케이스 가공 |
| 소프트웨어 | `firmware/`, `tools/`, `docs/software-design.md` | 추론 엔진, 변환기, UI 펌웨어 |
| 같이 | `sketches/`, `docs/plan.md`, `sd-card/` | 테스트 스케치, 일정, 공용 자료 |

### 브랜치와 PR

1. `main`은 항상 "업로드하면 동작하는" 상태로 둡니다. `main`에 직접 커밋하지 않습니다.
2. 작업은 브랜치를 따서 합니다. 이름은 담당 영역을 앞에 붙입니다.
   - 하드웨어: `hw/power-supply`, `hw/input-buffer`
   - 소프트웨어: `sw/s0-benchmark`, `sw/nam2namb`
   - 문서: `docs/pin-map`
3. 작업이 끝나면 Pull Request를 열고 **상대방이 한 번 보고 머지**합니다. 상대 영역을 건드렸으면 꼭 리뷰를 받습니다.
4. PR 설명에는 무엇을 바꿨는지와 **실제 보드/브레드보드에서 확인했는지**를 적습니다 (템플릿이 자동으로 뜹니다).

### 커밋 메시지

한국어로 짧게, 앞에 영역을 붙입니다.

```
hw: 입력 버퍼 브레드보드 측정값 추가
sw: 02 패스스루 48kHz 설정
docs: 핀 배치 확정
```

### 핀 배치는 한 곳에서 관리

핀 번호는 회로와 코드 양쪽에 영향을 줍니다. 핀을 바꿀 때는 **같은 PR에서** 아래를 함께 고칩니다.

1. [`docs/hardware-design.md`](docs/hardware-design.md)의 핀 배치 표
2. [`firmware/src/pins.h`](firmware/src/pins.h)
3. 해당 핀을 쓰는 `sketches/` 스케치의 상단 `#define`

### 할 일 관리

- 할 일과 버그는 GitHub **Issues**로 등록하고 담당자를 지정합니다. 라벨: `hardware`, `software`, `docs`, `question`.
- 측정 결과(전압, 잡음, CPU 사용률)는 `hardware/measurements/`나 `tools/benchmark_report.md`에 기록해서 둘 다 볼 수 있게 합니다.
- 설계 문서의 **(확인 필요)** 항목을 확인하면 문서를 **(확정)**으로 고치고 근거를 적습니다.

### 올리지 않는 것

- 빌드 결과물(`.pio/`, `.hex`), 개인 IDE 설정, 대용량 오디오 녹음 파일은 올리지 않습니다 (`.gitignore`에 있음).
- `.nam` 모델 파일은 저작권을 확인하고 올립니다. 기본적으로는 각자 TONE3000 등에서 받아서 씁니다.

## 라이선스

[MIT](LICENSE)
