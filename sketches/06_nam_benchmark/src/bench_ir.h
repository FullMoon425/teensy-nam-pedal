#pragma once
// 캐비닛 IR 컨볼루션: 균일 분할 overlap-save (분할 = 블록 128, FFT 256)
//
//   블록마다: [직전 블록, 이번 블록] 256샘플 → FFT → 주파수 지연선(FDL)에 저장
//             Y = Σ_p X(k-p) · H_p   (H_p = IR의 p번째 128샘플 조각을 256으로 0 채워 FFT)
//             y = IFFT(Y)의 뒤쪽 128샘플이 이번 블록 출력
//   추가 지연 없음 (블록 크기 = 분할 크기). 블록당 FFT 2번 + 복소 곱셈 P×129번.
//
// FFT: 보드는 CMSIS-DSP arm_rfft_fast_f32 (Teensy 툴체인 libarm_cortexM7lfsp_math.a),
//      PC는 같은 출력 형식의 단순 DFT (정확성 테스트용, 느림).
// 형식 (CMSIS rfft_fast): [X0.re, X(N/2).re, X1.re, X1.im, ..., X(N/2-1).re, X(N/2-1).im]
//
// IR 분할 스펙트럼 계산(setIr)은 오디오 밖(loop)에서만. process()는 오디오 인터럽트에서.

#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__arm__)
#include <arm_math.h>
#include <arm_common_tables.h>
#endif

namespace bench {

class RealFft256 {
public:
  static constexpr int N = 256;
#if defined(__arm__)
  // arm_rfft_fast_init_f32()는 길이별 switch로 모든 크기의 트위들 표(수십 KB)를 참조해 링커가 다 끌어옴.
  // Teensy 4는 const 데이터도 RAM1(DTCM)에 올리므로 RAM1을 약 100KB 잡아먹음 → 256용 표만 직접 연결.
  // 값은 CMSIS arm_rfft_fast_init_f32()의 256 경우와 같음.
  void init() {
    inst_.Sint.fftLen = N / 2;
    inst_.Sint.pTwiddle = twiddleCoef_128;
    inst_.Sint.pBitRevTable = armBitRevIndexTable128;
    inst_.Sint.bitRevLength = ARMBITREVINDEXTABLE_128_TABLE_LENGTH;
    inst_.fftLenRFFT = N;
    inst_.pTwiddleRFFT = (float32_t*)twiddleCoef_rfft_256;
  }
  // CMSIS는 입력 버퍼를 덮어쓰므로 in은 버려도 되는 버퍼여야 함
  void forward(float* in, float* out) { arm_rfft_fast_f32(&inst_, in, out, 0); }
  void inverse(float* in, float* out) { arm_rfft_fast_f32(&inst_, in, out, 1); }
private:
  arm_rfft_fast_instance_f32 inst_;
#else
  void init() {}
  void forward(float* in, float* out) {
    for (int k = 0; k <= N / 2; k++) {
      double re = 0, im = 0;
      for (int n = 0; n < N; n++) {
        const double a = -2.0 * M_PI * k * n / N;
        re += in[n] * cos(a);
        im += in[n] * sin(a);
      }
      if (k == 0) out[0] = (float)re;
      else if (k == N / 2) out[1] = (float)re;
      else { out[2 * k] = (float)re; out[2 * k + 1] = (float)im; }
    }
  }
  void inverse(float* in, float* out) {  // 1/N 스케일 포함 (CMSIS 역변환과 같게)
    for (int n = 0; n < N; n++) {
      double x = in[0] + in[1] * ((n & 1) ? -1.0 : 1.0);
      for (int k = 1; k < N / 2; k++) {
        const double a = 2.0 * M_PI * k * n / N;
        x += 2.0 * (in[2 * k] * cos(a) - in[2 * k + 1] * sin(a));
      }
      out[n] = (float)(x / N);
    }
  }
#endif
};

class IrConvolver {
public:
  static constexpr int B = 128;            // 분할 = 오디오 블록
  static constexpr int N = 2 * B;          // FFT 길이
  static constexpr int MAX_PARTS = 32;     // 최대 IR 4096샘플 (85ms)

  // 저장소는 밖에서 받음 (보드에서는 DMAMEM = RAM2에 둬서 A2-Lite의 DTCM과 겹치지 않게)
  // spec, fdl: 각각 MAX_PARTS * N floats
  void attach(float* spec, float* fdl) { H_ = spec; fdl_ = fdl; fft_.init(); }

  // IR 설정 (오디오가 이 객체를 안 쓰는 동안에만). len은 B의 배수로 올림, 최대 B*MAX_PARTS
  void setIr(const float* ir, int len) {
    if (len > B * MAX_PARTS) len = B * MAX_PARTS;
    parts_ = (len + B - 1) / B;
    float tmp[N];
    for (int p = 0; p < parts_; p++) {
      for (int i = 0; i < B; i++) {
        const int j = p * B + i;
        tmp[i] = (j < len) ? ir[j] : 0.0f;
      }
      for (int i = B; i < N; i++) tmp[i] = 0.0f;
      fft_.forward(tmp, &H_[p * N]);
    }
    reset();
  }
  void reset() {
    memset(fdl_, 0, sizeof(float) * MAX_PARTS * N);
    memset(prev_, 0, sizeof(prev_));
    pos_ = 0;
  }
  int parts() const { return parts_; }
  int length() const { return parts_ * B; }

  // n은 반드시 B (오디오 블록 128)
  void process(const float* in, float* out) {
    float buf[N];
    memcpy(buf, prev_, sizeof(float) * B);
    memcpy(buf + B, in, sizeof(float) * B);
    memcpy(prev_, in, sizeof(float) * B);

    // 이번 블록 스펙트럼을 FDL 현재 칸에
    float* X = &fdl_[pos_ * N];
    fft_.forward(buf, X);

    // Y = Σ X(k-p) · H_p
    float Y[N];
    memset(Y, 0, sizeof(Y));
    int slot = pos_;
    for (int p = 0; p < parts_; p++) {
      const float* __restrict x = &fdl_[slot * N];
      const float* __restrict h = &H_[p * N];
      float* __restrict y = Y;
      y[0] += x[0] * h[0];  // DC (실수)
      y[1] += x[1] * h[1];  // 나이퀴스트 (실수)
      for (int k = 2; k < N; k += 2) {
        const float xr = x[k], xi = x[k + 1], hr = h[k], hi = h[k + 1];
        y[k]     += xr * hr - xi * hi;
        y[k + 1] += xr * hi + xi * hr;
      }
      if (--slot < 0) slot = MAX_PARTS - 1;
    }
    if (++pos_ == MAX_PARTS) pos_ = 0;

    float yt[N];
    fft_.inverse(Y, yt);
    memcpy(out, yt + B, sizeof(float) * B);  // 앞 128은 순환 겹침이라 버림
  }

private:
  RealFft256 fft_;
  float* H_ = nullptr;
  float* fdl_ = nullptr;
  float prev_[B];
  int parts_ = 0;
  int pos_ = 0;
};

// 테스트·측정용 가짜 캐비닛 IR: 감쇠하는 잡음 (결정적)
inline void makeTestIr(float* ir, int len) {
  uint32_t s = 777u;
  for (int i = 0; i < len; i++) {
    s = s * 1664525u + 1013904223u;
    const float r = (float)(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    ir[i] = r * expf(-(float)i / (len * 0.2f)) * 0.1f;
  }
}

}  // namespace bench
