# 06_nam_benchmark (S0, 작업 예정)

프로젝트의 가장 큰 리스크인 "Teensy 4.1에서 NAM이 실시간으로 도는가"를 검증하는 스케치입니다.
소프트웨어 담당이 [software-design.md](../../docs/software-design.md) 11장의 **S0 프롬프트**로 만들어 이 폴더에 넣습니다.

## 할 일
1. Teensy Audio Library를 48kHz로 동작 (`AUDIO_SAMPLE_RATE_EXACT`, I2S/PLL4 설정). 사인파를 내보내 주파수를 측정해 검증
2. 더미 가중치로 아키텍처별 순전파를 돌리는 AudioStream 객체
   - A2-Lite (1순위), LSTM hidden 16 / 24, A1 WaveNet Nano, Feather
3. 600MHz / 816MHz, `std::tanh` / 근사 tanh 조합별 `AudioProcessorUsageMax()`를 시리얼로 출력
4. 결과를 [`tools/benchmark_report.md`](../../tools/benchmark_report.md)에 기록하고 "CPU 70% 이하" 기준으로 지원 아키텍처 확정

## 통과 기준
- 48kHz에서 02와 같은 패스스루가 정상 동작
- 지원 아키텍처 목록이 확정되어 docs의 **(벤치마크 후 결정)** 항목이 **(확정)**으로 바뀜
