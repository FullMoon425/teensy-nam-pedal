# 06_nam_benchmark (S0)

프로젝트의 가장 큰 리스크인 "Teensy 4.1에서 NAM이 실시간으로 도는가"를 검증하는 스케치입니다.
소프트웨어 담당이 [software-design.md](../../docs/software-design.md) 11장의 **S0 프롬프트**로 만들어 이 폴더에 넣습니다.

## 빌드 방법: 독립 PlatformIO 프로젝트

01~05와 달리 **PlatformIO로 빌드**합니다. 48kHz는 `-DAUDIO_SAMPLE_RATE_EXACT=48000.0f`를 코어와 Audio 라이브러리까지 같이 적용해야 하는데, Arduino IDE 스케치 안의 `#define`으로는 라이브러리에 적용되지 않기 때문입니다.

```
sketches/06_nam_benchmark/
  platformio.ini     build_flags에 -DAUDIO_SAMPLE_RATE_EXACT=48000.0f
  src/main.cpp       48kHz 검증 뼈대 (1kHz 사인 / 패스스루). 벤치마크 객체는 여기에 추가
```

메인 펌웨어(`firmware/`)에 env를 추가하지 않고 따로 둔 이유: 벤치마크용 더미 모델과 측정 코드가 메인 펌웨어와 섞이지 않고, 메인 펌웨어는 S0 검증 전까지 44.1kHz로 안전하게 남습니다. 벤치마크에서 검증한 추론 코드는 S1에서 `firmware/src/nam/`으로 옮깁니다.

명령은 저장소 루트에서 실행합니다 (VS Code에서는 이 폴더를 열고 상태바 아이콘을 써도 됩니다).

```bash
pio run -d sketches/06_nam_benchmark                 # 빌드
pio run -d sketches/06_nam_benchmark -t upload       # 업로드
pio device monitor -b 115200                         # 시리얼 모니터
```

`src/main.cpp`의 `TEST_SINE`이 1이면 LINE OUT L로 1kHz 사인이 나오고, 0이면 LINE IN L 패스스루입니다.

## 할 일
1. Teensy Audio Library를 48kHz로 동작. 방법은 [software-design.md 3장](../../docs/software-design.md#3-오디오-설정-확정--확인-필요)에 정리되어 있습니다.
   - `platformio.ini`의 `build_flags`에 `-DAUDIO_SAMPLE_RATE_EXACT=48000.0f` (I2S/PLL4는 Audio 라이브러리 `output_i2s.cpp`가 이 값으로 자동 계산)
   - `codec.enable()` 뒤에 `Codec48k::setSysFs48k()`로 SGTL5000 `CHIP_CLK_CTRL`을 `0x0008`(SYS_FS 48kHz)로 덮어쓰기 (`src/main.cpp`에 있음)
   - 1kHz 사인파를 내보내 주파수를 측정해 검증 (1000Hz 정상, 약 1088Hz 또는 919Hz면 설정 불일치). 결과는 `benchmark_report.md`의 "48kHz 설정 검증"에 기록
2. 더미 가중치로 아키텍처별 순전파를 돌리는 AudioStream 객체
   - A2-Lite (1순위), LSTM hidden 16 / 24, A1 WaveNet Nano, Feather
3. 600MHz / 816MHz, `std::tanh` / 근사 tanh 조합별 `AudioProcessorUsageMax()`를 시리얼로 출력
4. 결과를 [`tools/benchmark_report.md`](../../tools/benchmark_report.md)에 기록하고 "CPU 70% 이하" 기준으로 지원 아키텍처 확정

## 통과 기준
- 48kHz에서 02와 같은 패스스루가 정상 동작
- 지원 아키텍처 목록이 확정되어 docs의 **(벤치마크 후 결정)** 항목이 **(확정)**으로 바뀜
