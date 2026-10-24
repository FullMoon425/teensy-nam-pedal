# firmware

메인 NAM 펌웨어입니다. PlatformIO로 빌드합니다 (업로드 방법은 [루트 README](../README.md#방법-2-platformio-메인-펌웨어-06-벤치마크용)).

```
platformio.ini
src/main.cpp            설정·루프 (지금은 패스스루 뼈대)
src/pins.h              핀 배치 (회로와 코드가 공유하는 유일한 기준)
src/audio/              AudioEffectNAM: AudioStream 상속, 블록 단위 추론
src/nam/                경량 추론 엔진 (A2-Lite, LSTM, A1 WaveNet), .namb 포맷
src/dsp/                노이즈게이트, 바이쿼드 EQ, 게인, 소프트 클리퍼
src/storage/            ModelLoader, Presets, SystemState
src/ui/                 노브·인코더·풋스위치 입력, 페이지, 메뉴, 화면
test_pc/                PC 빌드 정확성 테스트
```

## 지켜야 할 규칙 (software-design.md 1장)
- 오디오 `update()` 안에서 SD 접근, `malloc/new`, `Serial`, 화면 갱신 금지
- 모든 버퍼는 정적 할당
- `src/nam/`, `src/dsp/`는 Arduino 헤더를 include하지 않음 (PC에서도 빌드해야 함)
- 핫루프는 `FASTRUN`, 성능 주장은 측정 코드와 함께
