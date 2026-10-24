# 05_sd_card

Teensy 4.1 **내장** microSD 슬롯(SDIO)을 확인합니다. 오디오실드의 SD 슬롯은 쓰지 않습니다.

## 준비
1. microSD(8~32GB)를 FAT32로 포맷
2. 저장소의 [`sd-card/`](../../sd-card) 안의 폴더(`models`, `presets`, `system`)를 카드 루트에 복사

## 통과 기준
- SD 용량과 `/models`, `/presets`, `/system` 목록이 찍힘
- `읽은 내용: teensy-nam-pedal sd test`
- 4KB 청크 최악 시간을 기록 (수 ms 이내면 loop()에서 비차단 로딩에 문제 없음)
  - `/models`에 미리 파일(변환한 `.namb`가 없으면 아무 파일)을 넣어 두면 `[기존 파일]` 결과가 같이 나옵니다. `[방금 쓴 파일]`은 낙관적인 값이라, **전원을 껐다 켠 직후 첫 실행의 `[기존 파일]` 값**을 기록합니다.
