# test_pc

`firmware/src/nam/`과 `firmware/src/dsp/`를 PC에서 그대로 빌드해, 공식 NAM으로 만든 기준 출력과 비교하는 테스트입니다 (S1).
목표: 최대 오차 1e-4 이하. 자세한 내용은 [software-design.md](../../docs/software-design.md) 9~10장.

예정 구성:
```
CMakeLists.txt
test_reference.cpp   tools/make_reference.py 출력과 비교
```
