# Teensy 4.1 NAM 앰프 모델러 페달 계획서

목표: 입대 전에 "실제로 페달보드에 올려 쓸 수 있는" 앰프 모델러 1대 완성.
세부 설계와 작업용 프롬프트는 두 문서로 나뉘어 있습니다.
- 회로: [hardware-design.md](hardware-design.md)
- 소프트웨어: [software-design.md](software-design.md)

원칙: **가장 큰 리스크(Teensy에서 NAM이 실시간으로 도는가)를 첫 주에 검증**하고, 나머지는 검증된 범위 안에서 만든다.

---

## 1. 핵심 판단: Teensy 4.1에서 NAM이 돌아가나?

Teensy 4.1 사양: Cortex-M7 600MHz(단정밀 FPU), RAM 1MB(그 중 512KB가 빠른 TCM), 플래시 8MB, 내장 microSD(SDIO), PSRAM 패드(선택).

NAM 모델은 아키텍처별로 연산량이 크게 다릅니다. 아래 수치는 레이어 구성으로 계산한 **추정치**이고, Phase 0 벤치마크로 확정해야 합니다.

| 모델 종류 | 샘플당 연산(대략) | 48kHz 환산 | Teensy 4.1 판단 |
|---|---|---|---|
| WaveNet **Standard** | 수만 MAC + tanh 수백 회 | 1 GMAC/s 이상 | **불가**. 600MHz M7의 현실적 처리량(수백 MMAC/s)을 넘음 |
| WaveNet **Lite** | Standard의 약 절반 | | 거의 불가 |
| WaveNet **Feather** | Standard의 수분의 1 | | 경계선. 오버클럭(816MHz)+최적화 필요할 수 있음 |
| WaveNet **Nano** | Standard의 1/10 안팎 | | **가능성 높음**. 1차 목표 |
| **LSTM** (hidden 8~24) | 1~3천 MAC | 수십~150 MMAC/s | **여유 있게 가능**. 안전한 대안 |
| **A2-Lite** (2026 신규) | 공개 수치 없음 | | **1순위 목표.** TONE3000 발표: 600MHz Cortex-M7(Teensy 4.1과 같은 급)에서 CPU 약 50%. Daisy Seed(480MHz)에서 약 53% 사용자 보고 |

결론:
- **1차 지원 대상: A2-Lite** (음질은 A1 Standard급, 임베디드용으로 설계됨). A1 Nano와 소형 LSTM은 예비. Feather는 벤치마크 결과를 보고 결정.
- A2 모델 파일 하나에 Full(8채널)과 Lite(3채널)가 같이 들어 있고, TONE3000에서 새로 학습하면 A2만 나옵니다. 그래서 새 모델은 재학습 없이 Lite 부분만 꺼내 쓰면 됩니다.
- A1 Standard와 A2-Full은 지원하지 않습니다. 예전 A1 Standard 모델을 쓰고 싶으면 A2로 재학습하세요.
- 메모리는 문제 없음: Nano/Feather 가중치는 수천~수만 개 float(수십 KB 수준). 모델 2개(현재 + 로딩 중)를 RAM에 동시에 들고 있어도 충분합니다.
- **샘플레이트**: NAM 모델은 48kHz로 학습됩니다. Teensy Audio Library 기본값은 44.1kHz이므로 **48kHz로 바꿔서 돌려야** 톤이 정확합니다(라이브러리의 `AUDIO_SAMPLE_RATE_EXACT` 수정 + I2S 클럭 설정). 이것도 Phase 0에서 같이 검증합니다.
- **추론 엔진**: 공식 NeuralAmpModelerCore는 Eigen, nlohmann/json, std::filesystem을 써서 Teensy에 그대로 올리기 무겁습니다. **WaveNet/LSTM 순전파만 직접 짠 경량 C++ 엔진**(CMSIS-DSP 활용)을 만들고, PC에서 공식 코어로 뽑은 기준 출력과 비교해 정확성을 검증하는 방식을 권장합니다.
- 참고: 같은 프로젝트의 Daisy Seed(480MHz M7)도 연산 체급은 비슷해서, Teensy에서 안 되는 모델은 Daisy에서도 대체로 안 됩니다. 반대로 Teensy에서 검증된 엔진 코드는 Daisy로 거의 그대로 옮길 수 있습니다.

### 모델 파일 형식(.nam / .namb)
- `.nam`은 JSON(가중치가 텍스트 숫자 배열)이라 SD에서 파싱하면 느리고 메모리를 많이 씁니다.
- **PC용 변환 스크립트(Python)로 `.nam` → `.namb`(자체 바이너리: 헤더 + 아키텍처 설정 + float32 가중치)** 로 바꿔서 SD에 넣는 구조로 정했습니다. 페달은 `.namb`만 읽습니다.
- 변환 시 지원 불가 아키텍처(Standard 등)는 PC 단계에서 경고하고 거부합니다.

---

## 2. 기능 정의

| 요구사항 | 구현 방식 |
|---|---|
| 외부 학습 모델 불러오기/저장 | microSD `/models/*.namb`. PC에서 변환 후 복사. (선택) USB MTP로 SD를 빼지 않고 PC에서 드래그&드롭 |
| 저장된 모델 전환 | 프리셋 단위로 전환. 프리셋 = 모델 + 노브값. 로딩 중에도 기존 모델로 소리 유지, 로딩 완료 후 짧은 크로스페이드로 교체(팝 노이즈 방지) |
| 작은 노브 3개 | B10K 포텐셔미터 → Teensy 아날로그 입력(3.3V 기준). 페이지별 기능 전환 |
| 큰 노브 1개 | **로터리 인코더(EC11, 푸시 스위치 포함)**. 회전=선택/값 변경, 누름=확인/페이지, 길게 누름=저장/메뉴 |
| 스크린 | 1.3" 128x64 OLED(SH1106/SSD1306, SPI 권장) |
| 1/4" TS 입출력 | 입력: 고임피던스(1MΩ) 버퍼. 출력: 저임피던스 버퍼 |
| FX 루프 Send/Return | 아날로그 루프(아래 신호 흐름 참고). Return은 스위칭 잭으로, 아무것도 안 꽂으면 자동 연결 |
| 풋스위치 1개 | 모멘터리(소프트 터치) 스위치. **짧게=바이패스 On/Off, 길게=다음 프리셋** |
| 전원 | 9V DC 센터 마이너스(아래 근거) |

### 노브 페이지 (인코더 누름으로 전환)
- **AMP 페이지**: Input Gain / Bass / Treble
- **EQ 페이지**: Bass / Mid / Treble
- **OUT 페이지**: Output Level / Noise Gate Threshold / (Mid 또는 Presence)

포텐셔미터는 절대 위치라서 페이지·프리셋 전환 시 값이 튀는 문제가 있습니다. **"캐치(soft takeover)" 방식**으로 정했습니다: 노브가 저장된 값을 지나칠 때까지는 값이 바뀌지 않고, 화면에 현재 노브 위치와 저장값을 같이 표시합니다.

### 화면 구성 예시
```
[03] Plexi Crunch        BYP
model: plexi_nano.namb
AMP  Gain 6.2 Bass 5.0 Treb 7.1
CPU 61%                 48k
```

---

## 3. 신호 흐름

```
IN 잭 → 입력 버퍼(1MΩ, 9V 바이어스) → 코덱 ADC
  → [디지털] 노이즈게이트 → Input Gain → NAM 모델 → EQ(바이쿼드) → Output Level
  → 코덱 DAC → 출력 버퍼 → SEND 잭
                       RETURN 잭(스위칭, 미삽입 시 SEND와 내부 연결) → 출력 버퍼 → OUT 잭
```
- FX 루프는 **아날로그, 앰프 모델 뒤(앰프의 이펙트 루프와 같은 위치)**에 둡니다. 딜레이·리버브를 앰프 뒤에 거는 일반적인 용도에 맞고, CPU를 안 쓰고, 코덱 채널도 추가로 필요 없습니다.
- 바이패스는 우선 **디지털 바이패스**(입력을 그대로 출력)로 구현합니다. 시간이 남으면 릴레이 트루 바이패스(전원 꺼져도 소리 통과)를 추가합니다.
- 캐비닛 IR: NAM 모델이 앰프만 캡처한 경우 캐비닛 시뮬이 필요합니다. **MVP에서는 "앰프+캐비닛 통째로 캡처한 모델" 사용을 전제**로 하고, IR 컨볼루션은 CPU 여유가 남을 때의 확장 기능으로 둡니다.

---

## 4. 전원 규격: 9V DC 센터 마이너스, 500mA 이상 권장

근거:
- 기타 페달 표준(Boss 방식 2.1mm 배럴, 센터 마이너스)이라 기존 파워서플라이·데이지체인을 그대로 씁니다.
- 예상 소비전류: Teensy 4.1(600MHz) 약 100mA + 오디오 코덱·OLED·오피앰프 약 50~80mA → **약 150~200mA**. 여유를 두어 9V 300mA 이상 출력에서 동작, 권장 500mA. (디지털 페달이 보통 이 수준을 요구합니다.)

내부 전원 구성:
- 역극성 보호: P-MOSFET(또는 쇼트키 다이오드) + 입력 전해 100µF + 필름 0.1µF
- 9V → **5V 레귤레이터**(L7805/LM2940, TO-220) → Teensy VIN. 손실은 약 0.8W로 소형 방열 정도면 충분하고, 스위칭 레귤레이터보다 잡음이 적습니다.
- 오피앰프는 9V 직접 사용, 4.5V 바이어스(Vref) 생성.
- **Teensy 뒷면 VUSB–VIN 연결 패드를 끊어야** USB 꽂은 상태에서 외부 전원과 충돌하지 않습니다(펌웨어 업로드 중에도 안전).
- 접지는 스타 접지, 디지털 접지와 오디오 접지를 한 점에서 합칩니다.

---

## 5. 하드웨어 구성(BOM 초안, 상세는 hardware-design.md)

| 부품 | 권장 | 비고 |
|---|---|---|
| MCU | Teensy 4.1 | 헤더 핀 납땜 |
| 오디오 코덱 | **PJRC Audio Shield Rev D (SGTL5000)** | 라이브러리 지원이 가장 잘 돼 있어 일정에 유리. 음질 아쉬우면 이후 PCM3060/WM8731 보드로 업그레이드 |
| 입력/출력 버퍼 | TL072 또는 OPA2134(듀얼 오피앰프) 2개 | 입력 1MΩ, 출력 직렬 1kΩ |
| 노브 | B10K 포텐셔미터 3개 + EC11 인코더 1개 | 포트 와이퍼에 0.1µF 붙여 잡음 감소 |
| 스크린 | 1.3" OLED 128x64 SPI | 오디오 실드는 SD를 안 쓰므로 SPI 공유 가능(내장 SD 슬롯은 SDIO라 별개) |
| 잭 | 1/4" 모노 잭 4개(IN, OUT, SEND, RETURN-스위칭형) | |
| 풋스위치 | 모멘터리 소프트터치 SPST | |
| 전원 | 2.1mm DC 잭, P-MOSFET, L7805, 캐패시터류 | |
| 저장장치 | microSD(8~32GB, FAT32) | Teensy 내장 슬롯 |
| 케이스 | **Hammond 1590XX** (또는 1590BB2) | 노브 4 + 화면 + 잭 4 + 풋스위치를 넣으려면 1590BB는 빡빡함 |

핀 배치는 Phase 1에서 확정합니다. 오디오 실드가 쓰는 핀(I2S 7/8/20/21/23, I2C 18/19)을 피해서 배치해야 합니다.

---

## 6. 소프트웨어 구조(상세는 software-design.md)

```
firmware/
  src/main.cpp            설정·루프
  audio/AudioEffectNAM    Teensy AudioStream 상속, 블록 단위 추론
  nam/WaveNet, LSTM       경량 추론 엔진 (float32, CMSIS-DSP)
  nam/ModelLoader         .namb 파싱, 더블 버퍼, 안전한 교체
  dsp/Gate, EQ, Gain      전후처리
  ui/Knobs, Encoder, Footswitch, Display, Pages
  storage/Presets         SD의 /presets/*.json 읽기/쓰기
tools/
  nam2namb.py             .nam → .namb 변환 + 아키텍처 검증
  make_reference.py       공식 NAM 코어로 기준 입출력 생성(정확성 테스트용)
```
- 빌드: PlatformIO(Teensy 플랫폼). 오디오 처리는 인터럽트에서, SD/화면/노브는 `loop()`에서. 오디오 경로에서는 절대 SD 접근·동적 할당 금지.
- 가중치와 상태 버퍼는 DTCM/OCRAM에 배치, 핫루프는 ITCM(`FASTRUN`).
- 블록 크기 128샘플 @48kHz → 버퍼 지연 약 2.7ms(입출력 합쳐 약 5~6ms). 연주감에 문제 없는 수준.
- CPU 사용률은 `AudioProcessorUsageMax()`로 화면에 표시.

---

## 7. 단계별 일정 (주 단위, 약 8주 기준)

입대 날짜에 맞춰 주 단위를 줄이거나 늘리면 됩니다. **★ 표시가 MVP**(이것만 되면 쓸 수 있는 페달).

| 주차 | 단계 | 완료 기준 |
|---|---|---|
| 1 | **Phase 0 ★ 실현 가능성 검증** | Teensy+오디오실드 48kHz 패스스루 동작. A2-Lite(1순위)·A1 Nano·LSTM 더미 가중치로 CPU 사용률 측정. 지원 범위 결정 |
| 2 | Phase 1 ★ 아날로그 프론트엔드 | 브레드보드에서 입력/출력 버퍼 + 코덱으로 기타 소리 깨끗하게 통과 |
| 2~3 | Phase 2 ★ 추론 엔진 | `nam2namb.py` 완성. PC 기준 출력과 오차 1e-4 이하. 실제 기타로 모델 소리 확인 |
| 4 | Phase 3 ★ 저장·전환 | SD에서 모델 로드, 프리셋 저장/불러오기, 끊김 없는 전환 |
| 5 | Phase 4 ★ UI | 노브 3 + 인코더 + 화면 + 풋스위치, 페이지·캐치 동작 |
| 6 | Phase 5 ★ 하드웨어 마감 | 전원부, FX 루프, 만능기판(또는 PCB) 조립, 케이스 가공 |
| 7 | Phase 6 다듬기 | 노이즈 잡기, 노브 커브 조정, 노이즈게이트 튜닝, 실사용 테스트 |
| 8 | 여유/확장 | USB MTP, 릴레이 트루바이패스, 캐비닛 IR, Feather 최적화 |

시간이 모자라면 자르는 순서: 캐비닛 IR → 릴레이 바이패스 → USB MTP → EQ 페이지(Bass/Treble만 남김).
PCB 주문(JLCPCB 등)은 배송 1~2주가 걸리므로, 시간이 촉박하면 **만능기판 + 기성 모듈**로 마감하는 쪽을 권장합니다.

---

## 8. 리스크와 대응

| 리스크 | 대응 |
|---|---|
| 원하는 모델이 A1 Standard라 안 돌아감 | A2로 재학습(A2 파일의 Lite 부분 사용) |
| A2-Lite 구조 공개 정보 부족 | NAM 공식 코어의 A2 구현 소스를 기준으로 재구현. Daisy용 커뮤니티 구현(아래 출처) 참고 |
| 48kHz 설정이 꼬임 | 최악의 경우 44.1kHz로 돌리고 약간의 톤 차이 감수(또는 학습을 44.1kHz로) |
| SGTL5000 잡음/헤드룸 부족 | 입력 게인 단계 조정, 필요 시 코덱 보드 교체 |
| 모델 교체 시 팝 노이즈 | 더블 버퍼 + 10~20ms 크로스페이드 |
| 디지털 잡음(화면·SD 접근 시 '찌직') | 스타 접지, 레귤레이터 분리, 화면 갱신 주기 제한(예: 20fps) |


---

## 참고 출처 (A2)
- [Introducing NAM Architecture 2 (TONE3000)](https://www.tone3000.com/blog/introducing-neural-amp-modeler-nam-architecture-2-a2)
- [NAM A2: The Complete Guide](https://www.tone3000.com/guides/nam-a2-the-complete-guide)
- [NAM A2 on Daisy Seed (Daisy 포럼)](https://community.daisy.audio/t/nam-a2-on-daisy-seed/9186)
