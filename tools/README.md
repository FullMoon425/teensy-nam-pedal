# tools

PC에서 돌리는 파이썬 도구입니다 (S1).

| 파일 | 내용 | 상태 |
|---|---|---|
| `nam2namb.py` | `.nam`(JSON) → `.namb`(자체 바이너리) 변환, 지원 아키텍처 검증 | 작업 예정 |
| `make_reference.py` | 공식 NAM 파이썬 패키지로 기준 입출력 wav 생성 | 작업 예정 |
| [`benchmark_report.md`](benchmark_report.md) | S0 CPU 벤치마크 결과 기록 | 양식 |

`.namb` 포맷 명세는 [software-design.md](../docs/software-design.md) 5-1절을 따릅니다.
변환 결과물(`.namb`)과 wav는 `tools/out/`에 두면 git에 올라가지 않습니다.
