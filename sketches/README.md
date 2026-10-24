# 단계별 테스트 스케치

부품을 하나씩 붙이면서 동작을 확인하는 짧은 스케치입니다. **번호 순서대로** 올리고, 각 폴더 README의 "통과 기준"을 만족하면 다음으로 넘어갑니다.
업로드 방법은 저장소 루트 [README](../README.md#업로드-방법)의 "방법 1: Arduino IDE + Teensyduino"를 보세요.

| 스케치 | 필요한 부품 | 확인할 것 |
|---|---|---|
| [01_blink](01_blink) | Teensy 4.1 | 업로드 환경, 시리얼 출력 |
| [02_audio_passthrough](02_audio_passthrough) | + 오디오실드 | LINE IN → LINE OUT 소리 통과, CPU/메모리 사용량 |
| [03_knobs_switches](03_knobs_switches) | + 포트 3, 인코더, 풋스위치, LED | 모든 조작 입력 인식 |
| [04_oled_display](04_oled_display) | + OLED | 화면 표시, 갱신 속도 |
| [05_sd_card](05_sd_card) | + microSD | 내장 SD 읽기/쓰기, `/models` 목록 |
| [06_nam_benchmark](06_nam_benchmark) | 02와 같음 | 48kHz 설정, 모델별 CPU 사용률 (S0) |

핀 번호는 각 스케치 상단의 `#define`에 있습니다. 핀을 바꾸면 [`firmware/src/pins.h`](../firmware/src/pins.h)와 [`docs/hardware-design.md`](../docs/hardware-design.md)도 같이 고치세요.
