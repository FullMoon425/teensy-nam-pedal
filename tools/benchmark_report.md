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
- 방법:
- 측정 주파수 (1kHz 사인 출력):

## 결론
- 지원 아키텍처:
- CPU 클럭:
