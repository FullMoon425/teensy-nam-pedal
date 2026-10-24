# S0 벤치마크 결과

측정 스케치: `sketches/06_nam_benchmark`
기준: `AudioProcessorUsageMax()` **70% 이하**면 지원.

| 날짜 | 아키텍처 | 설정 | 클럭 | tanh | 블록 | CPU max % | 판정 |
|---|---|---|---|---|---|---|---|
| | A2-Lite | | 600MHz | std | 128 | | |
| | A2-Lite | | 600MHz | 근사 | 128 | | |
| | LSTM | hidden 16 | 600MHz | std | 128 | | |
| | LSTM | hidden 24 | 600MHz | std | 128 | | |
| | A1 Nano | | 600MHz | 근사 | 128 | | |
| | A1 Feather | | 816MHz | 근사 | 128 | | |

## 48kHz 설정 검증
- 방법: `sketches/06_nam_benchmark` (PlatformIO, `-DAUDIO_SAMPLE_RATE_EXACT=48000.0f` + `Codec48k::setSysFs48k()`)
- 시리얼 출력 `AUDIO_SAMPLE_RATE_EXACT`:
- 측정 주파수 (1kHz 사인 출력, 1000Hz 정상 / 약 1088Hz·919Hz면 불일치):
- SYS_FS를 44.1kHz로 둔 경우(`setSysFs48k()` 생략) 차이:
- Arduino IDE 전역 정의 방법:

## 결론
- 지원 아키텍처:
- CPU 클럭:
