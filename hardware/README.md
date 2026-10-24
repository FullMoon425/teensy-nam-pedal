# hardware

회로 담당 폴더입니다. 설계 근거는 [docs/hardware-design.md](../docs/hardware-design.md)에 있고, 여기에는 실제 결과물을 둡니다.

```
schematics/     회로도 (KiCad 프로젝트 또는 손그림 사진/PDF), 만능기판 레이아웃
measurements/   측정 기록 (전압, 전류, 잡음, 클리핑 레벨)
photos/         브레드보드·조립 사진
```

## 제작 단계 (hardware-design.md 7장)
| 단계 | 내용 | 완료 기준 |
|---|---|---|
| H0 | Teensy + 오디오실드 납땜 | `sketches/01`, `02` 통과 |
| H1 | 브레드보드: 전원부 → Vref → 입력/출력 버퍼 | 9V 어댑터로 기타 → 앰프 소리 깨끗 |
| H2 | 조작계·OLED 연결, 핀 배치 확정 | `sketches/03`, `04` 통과, `firmware/src/pins.h` 확정 |
| H3 | 만능기판 이식 | 브레드보드와 같은 동작 |
| H4 | 케이스 가공, 조립 | 점검 체크리스트 통과 |

## 측정 기록 양식
`measurements/YYYY-MM-DD_항목.md` 파일로 남깁니다. 예: `2026-11-02_power-supply.md`

```
- 측정 대상:
- 조건 (어댑터, 부하):
- 결과 (전압/전류/파형):
- 판단 / 다음 할 일:
```
