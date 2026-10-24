# sd-card

microSD(FAT32) 루트에 그대로 복사할 폴더 구조입니다 ([software-design.md](../docs/software-design.md) 5장).

```
models/     *.namb  (tools/nam2namb.py로 변환한 모델. git에는 올리지 않음)
presets/    01_clean.json ...  (프리셋 = 모델 파일명 + 노브값 0~1)
system/     state.json (마지막 프리셋, 바이패스 상태), settings.json
```
이 README 파일은 카드에 복사하지 않아도 됩니다.
