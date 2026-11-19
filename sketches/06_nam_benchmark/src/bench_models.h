#pragma once
// 06 벤치마크용 NAM 순전파 (더미 가중치)
// Arduino 헤더를 쓰지 않으므로 PC에서도 그대로 빌드됩니다 (firmware/src/nam/ 으로 옮길 때 같은 구조를 씀).
//
// 구조 근거 (공식 소스, 확인한 커밋):
//   A2-Lite    NeuralAmpModelerCore d3189ed  NAM/wavenet/a2_fast.{h,cpp}
//              23층, 채널 3(= bottleneck), 커널 6 (14·15번째 층만 15), LeakyReLU 0.01, 게이트·FiLM 없음,
//              layer1x1 있음, 헤드 = 커널 16 컨볼루션(채널 3 → 1, bias), head_scale은 가중치 마지막 값
//   A1 WaveNet NAM/wavenet/model.cpp (LayerArray), 크기는 neural-amp-modeler v0.12.3 nam/train/core.py get_wavenet_config
//              배열 2개, 커널 3, Tanh, 게이트 없음, head_scale 0.02
//   LSTM       NAM/lstm.cpp (게이트 순서 i, f, g, o), 크기는 같은 core.py get_lstm_config
//
// 이 파일은 "연산량과 메모리 접근 형태"를 공식과 같게 만든 벤치마크입니다.
// 가중치 로딩 순서 검증은 S1(PC 기준 출력 비교)에서 합니다.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace bench {

constexpr int kMaxFrames = 128;  // AUDIO_BLOCK_SAMPLES

// ---------------------------------------------------------------- 활성화
// 공식 NAM/activations.h 의 fast_tanh, fast_sigmoid 그대로
inline float fastTanh(float x) {
  const float ax = fabsf(x);
  const float x2 = x * x;
  return (x * (2.45550750702956f + 2.45550750702956f * ax + (0.893229853513558f + 0.821226666969744f * ax) * x2)
          / (2.44506634652299f + (2.44506634652299f + x2) * fabsf(x + 0.814642734961073f * x * ax)));
}
template <bool Fast> inline float actTanh(float x) { return Fast ? fastTanh(x) : tanhf(x); }
template <bool Fast> inline float actSigmoid(float x) {
  return Fast ? 0.5f * (fastTanh(0.5f * x) + 1.0f) : 1.0f / (1.0f + expf(-x));
}

// ---------------------------------------------------------------- 정적 풀
// 링버퍼를 부팅 때 잡은 정적 배열에서 나눠 줍니다. 모델을 바꿀 때 reset 후 다시 나눔.
// 실행 중 malloc/new 없음.
class Pool {
public:
  Pool(float* mem, int capacity) : mem_(mem), cap_(capacity) {}
  void reset() { used_ = 0; failed_ = false; }
  float* take(int n) {
    n = (n + 3) & ~3;  // 16바이트 정렬 유지
    if (used_ + n > cap_) { failed_ = true; return mem_; }  // 실패해도 죽지 않게, build()가 false를 돌려줌
    float* p = mem_ + used_;
    used_ += n;
    memset(p, 0, sizeof(float) * n);
    return p;
  }
  int used() const { return used_; }
  int capacity() const { return cap_; }
  bool failed() const { return failed_; }
private:
  float* mem_;
  int cap_;
  int used_ = 0;
  bool failed_ = false;
};

// 더미 가중치용 난수 (결정적, 매번 같은 값)
struct Rng {
  uint32_t s = 12345u;
  float uni() {  // -1 ~ 1
    s = s * 1664525u + 1013904223u;
    return (float)(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
  }
  void fill(float* p, int n, float scale) { for (int i = 0; i < n; i++) p[i] = uni() * scale; }
};

// ---------------------------------------------------------------- 링버퍼
// 프레임 단위(C채널 묶음) 링. 길이 R = lookback + 블록, 뒤에 블록 길이만큼 앞부분 복사본(미러)을 둬서
// 어느 탭이든 이번 블록 n프레임을 "끊김 없이 연속"으로 읽을 수 있게 합니다 (샘플마다 % 연산 없음).
template <int C>
struct Ring {
  float* buf = nullptr;
  int R = 0;
  int wp = 0;

  void init(Pool& pool, int lookback) {
    R = lookback + kMaxFrames;
    buf = pool.take((R + kMaxFrames) * C);
    wp = 0;
  }
  void clear() {
    memset(buf, 0, sizeof(float) * (R + kMaxFrames) * C);
    wp = 0;
  }
  void write(const float* src, int n) {
    for (int f = 0; f < n; f++) {
      float* d = &buf[wp * C];
      for (int c = 0; c < C; c++) d[c] = src[f * C + c];
      if (wp < kMaxFrames) {
        float* m = &buf[(R + wp) * C];
        for (int c = 0; c < C; c++) m[c] = src[f * C + c];
      }
      if (++wp == R) wp = 0;
    }
  }
  // write(n) 직후 호출. 이번 블록 첫 프레임보다 back 샘플 앞의 위치. 거기서부터 n프레임이 연속.
  const float* at(int n, int back) const {
    int s = wp - n - back;
    if (s < 0) s += R;  // 최소값이 -(n + lookback) = -R 이므로 한 번이면 충분
    return &buf[s * C];
  }
};

// ---------------------------------------------------------------- 모델 공통
class Model {
public:
  virtual ~Model() {}
  virtual const char* name() const = 0;
  virtual const char* config() const = 0;   // 보고서 "설정" 칸
  virtual bool usesTanh() const { return true; }
  virtual int macsPerSample() const = 0;    // 곱셈-누산 수 (bias 덧셈 제외)
  virtual int tanhPerSample() const = 0;    // tanh 호출 수 (sigmoid는 tanh 1회로 셈)
  virtual bool build(Pool& pool) = 0;       // 링 할당 + 더미 가중치. 오디오가 이 모델을 안 쓰는 동안에만
  virtual void reset() = 0;                 // 상태 0
  virtual void process(const float* in, float* out, int n, bool fastTanh) = 0;
};

// ================================================================ A2-Lite
class A2Lite : public Model {
public:
  static constexpr int C = 3;
  static constexpr int kLayers = 23;
  static constexpr int kHeadK = 16;
  static constexpr float kSlope = 0.01f;

  const char* name() const override { return "A2-Lite"; }
  const char* config() const override { return "ch3, 23층, LeakyReLU"; }
  bool usesTanh() const override { return false; }
  int macsPerSample() const override {
    int m = C;  // rechannel
    for (int i = 0; i < kLayers; i++) m += kK[i] * C * C + C + C * C;  // conv + mixin + layer1x1
    return m + kHeadK * C;  // head
  }
  int tanhPerSample() const override { return 0; }

  bool build(Pool& pool) override {
    Rng r;
    r.fill(rech_, C, 1.0f);
    for (int i = 0; i < kLayers; i++) {
      Layer& L = L_[i];
      L.K = kK[i];
      L.D = kD[i];
      L.ring.init(pool, (L.K - 1) * L.D);
      const float s = 0.6f / sqrtf((float)(L.K * C));
      r.fill(L.w, L.K * C * C, s);
      r.fill(L.cb, C, 0.05f);
      r.fill(L.mix, C, 0.5f);
      r.fill(L.l1w, C * C, 0.3f);
      r.fill(L.l1b, C, 0.05f);
    }
    headRing_.init(pool, kHeadK - 1);
    r.fill(&headW_[0][0], kHeadK * C, 0.2f);
    headB_ = 0.0f;
    headScale_ = 0.01f;
    return !pool.failed();
  }
  void reset() override {
    for (auto& L : L_) L.ring.clear();
    headRing_.clear();
  }

  void process(const float* in, float* out, int n, bool) override {
    for (int f = 0; f < n; f++) {
      const float x = in[f];
      for (int c = 0; c < C; c++) lin_[f * C + c] = rech_[c] * x;
    }
    memset(hsum_, 0, sizeof(float) * n * C);
    for (int i = 0; i < kLayers; i++) layer(L_[i], in, n);
    head(out, n);
  }

private:
  static constexpr int kK[kLayers] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
  static constexpr int kD[kLayers] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239};

  struct Layer {
    int K = 0, D = 0;
    float w[15 * C * C];   // 탭마다 열 우선 (C×C): w[k*9 + j*3 + i], j=입력, i=출력 (a2_fast와 같음)
    float cb[C], mix[C];
    float l1w[C * C];      // 열 우선: l1w[b*3 + c], b=bottleneck, c=출력
    float l1b[C];
    Ring<C> ring;
  };

  // a2_fast.cpp _layer_forward_k 의 Channels==3 경로와 같은 계산 순서.
  // 최적화 메모 (보드 측정으로 확인한 것):
  //  - 포인터에 __restrict: 없으면 z_에 쓸 때마다 src를 다시 읽음 (프레임당 load 9번 → 3번)
  //  - 입력을 지역 변수로 한 번 읽고, 출력 3개를 번갈아 누적: FPU 대기(의존 사슬) 줄임
  //  - LeakyReLU = fmaxf(a, 0.01a): 분기·VMRS 없이 VMAXNM 하나 (음수 기울기 0~1이면 같은 값)
  void layer(Layer& L, const float* __restrict cond, int n) {
    L.ring.write(lin_, n);
    const int K = L.K, D = L.D;
    float* __restrict z = z_;
    // 탭 0: bias로 시작
    {
      const float* __restrict wk = &L.w[0];
      const float* __restrict src = L.ring.at(n, (K - 1) * D);
      const float w0 = wk[0], w1 = wk[1], w2 = wk[2], w3 = wk[3], w4 = wk[4], w5 = wk[5], w6 = wk[6], w7 = wk[7], w8 = wk[8];
      const float b0 = L.cb[0], b1 = L.cb[1], b2 = L.cb[2];
      for (int f = 0; f < n; f++) {
        const float s0 = src[f * C], s1 = src[f * C + 1], s2 = src[f * C + 2];
        float a0 = b0 + w0 * s0, a1 = b1 + w1 * s0, a2 = b2 + w2 * s0;
        a0 += w3 * s1; a1 += w4 * s1; a2 += w5 * s1;
        a0 += w6 * s2; a1 += w7 * s2; a2 += w8 * s2;
        z[f * C] = a0; z[f * C + 1] = a1; z[f * C + 2] = a2;
      }
    }
    for (int k = 1; k < K - 1; k++) {
      const float* __restrict wk = &L.w[k * 9];
      const float* __restrict src = L.ring.at(n, (K - 1 - k) * D);
      const float w0 = wk[0], w1 = wk[1], w2 = wk[2], w3 = wk[3], w4 = wk[4], w5 = wk[5], w6 = wk[6], w7 = wk[7], w8 = wk[8];
      for (int f = 0; f < n; f++) {
        const float s0 = src[f * C], s1 = src[f * C + 1], s2 = src[f * C + 2];
        float a0 = z[f * C], a1 = z[f * C + 1], a2 = z[f * C + 2];
        a0 += w0 * s0; a1 += w1 * s0; a2 += w2 * s0;
        a0 += w3 * s1; a1 += w4 * s1; a2 += w5 * s1;
        a0 += w6 * s2; a1 += w7 * s2; a2 += w8 * s2;
        z[f * C] = a0; z[f * C + 1] = a1; z[f * C + 2] = a2;
      }
    }
    // 마지막 탭 + mixin + LeakyReLU + 헤드 누적 + layer1x1 잔차
    const float* __restrict wk = &L.w[(K - 1) * 9];
    const float* __restrict src = L.ring.at(n, 0);
    const float w0 = wk[0], w1 = wk[1], w2 = wk[2], w3 = wk[3], w4 = wk[4], w5 = wk[5], w6 = wk[6], w7 = wk[7], w8 = wk[8];
    const float m0 = L.mix[0], m1 = L.mix[1], m2 = L.mix[2];
    const float l0 = L.l1w[0], l1 = L.l1w[1], l2 = L.l1w[2], l3 = L.l1w[3], l4 = L.l1w[4], l5 = L.l1w[5],
                l6 = L.l1w[6], l7 = L.l1w[7], l8 = L.l1w[8];
    const float lb0 = L.l1b[0], lb1 = L.l1b[1], lb2 = L.l1b[2];
    float* __restrict hs = hsum_;
    float* __restrict li = lin_;
    for (int f = 0; f < n; f++) {
      const float s0 = src[f * C], s1 = src[f * C + 1], s2 = src[f * C + 2];
      const float cf = cond[f];
      float a0 = z[f * C], a1 = z[f * C + 1], a2 = z[f * C + 2];
      a0 += w0 * s0; a1 += w1 * s0; a2 += w2 * s0;
      a0 += w3 * s1; a1 += w4 * s1; a2 += w5 * s1;
      a0 += w6 * s2; a1 += w7 * s2; a2 += w8 * s2;
      a0 += m0 * cf; a1 += m1 * cf; a2 += m2 * cf;
      a0 = fmaxf(a0, a0 * kSlope);
      a1 = fmaxf(a1, a1 * kSlope);
      a2 = fmaxf(a2, a2 * kSlope);
      hs[f * C] += a0; hs[f * C + 1] += a1; hs[f * C + 2] += a2;
      float r0 = lb0, r1 = lb1, r2 = lb2;
      r0 += l0 * a0; r1 += l1 * a0; r2 += l2 * a0;
      r0 += l3 * a1; r1 += l4 * a1; r2 += l5 * a1;
      r0 += l6 * a2; r1 += l7 * a2; r2 += l8 * a2;
      li[f * C] += r0; li[f * C + 1] += r1; li[f * C + 2] += r2;
    }
  }

  void head(float* out, int n) {
    headRing_.write(hsum_, n);
    // 링은 탭마다 n프레임 연속만 보장하므로 탭별로 시작 위치를 따로 구함
    const float* src[kHeadK];
    for (int k = 0; k < kHeadK; k++) src[k] = headRing_.at(n, kHeadK - 1 - k);
    // 탭 바깥, 프레임 안쪽 (탭마다 가중치 3개를 레지스터에 두고 n프레임에 누적)
    float* __restrict o = out;
    for (int f = 0; f < n; f++) o[f] = headB_;
    for (int k = 0; k < kHeadK; k++) {
      const float* __restrict s = src[k];
      const float h0 = headW_[k][0], h1 = headW_[k][1], h2 = headW_[k][2];
      for (int f = 0; f < n; f++) o[f] += h0 * s[f * C] + h1 * s[f * C + 1] + h2 * s[f * C + 2];
    }
    for (int f = 0; f < n; f++) o[f] *= headScale_;
  }

  Layer L_[kLayers];
  float rech_[C];
  float headW_[kHeadK][C];
  float headB_ = 0.0f, headScale_ = 0.01f;
  Ring<C> headRing_;
  float lin_[kMaxFrames * C];
  float hsum_[kMaxFrames * C];
  float z_[kMaxFrames * C];
};

// ================================================================ A1 WaveNet
// LayerArray 하나. IN: 입력 채널, C: 채널, HS: 헤드 출력 채널, NL: 층 수, 커널 3 고정.
// 층: z = conv(x) + mixin*cond → tanh → head += z, x += layer1x1(z)
// 배열 끝: headOut = head_rechannel(head) (1x1, C → HS)
template <int IN, int C, int HS, int NL>
class A1Array {
public:
  static constexpr int K = 3;
  static int macs() { return IN * C + NL * (K * C * C + C + C * C) + C * HS; }
  static int tanhs() { return NL * C; }

  void build(Pool& pool, Rng& r, const int* dil, bool headBias) {
    r.fill(rech_, C * IN, 1.0f / sqrtf((float)IN));
    for (int i = 0; i < NL; i++) {
      Layer& L = L_[i];
      L.D = dil[i];
      L.ring.init(pool, (K - 1) * L.D);
      r.fill(L.w, K * C * C, 0.8f / sqrtf((float)(K * C)));
      r.fill(L.cb, C, 0.05f);
      r.fill(L.mix, C, 0.5f);
      r.fill(L.l1w, C * C, 0.5f / sqrtf((float)C));
      r.fill(L.l1b, C, 0.05f);
    }
    r.fill(headW_, HS * C, 0.3f);
    if (headBias) r.fill(headB_, HS, 0.05f); else memset(headB_, 0, sizeof(headB_));
  }
  void reset() { for (auto& L : L_) L.ring.clear(); }

  // layerIn: n×IN, cond: n, headIn: n×C 또는 nullptr, layerOut: n×C 또는 nullptr, headOut: n×HS
  template <bool Fast>
  void process(const float* layerIn, const float* cond, const float* headIn, float* layerOut, float* headOut, int n) {
    for (int f = 0; f < n; f++)
      for (int c = 0; c < C; c++) {
        float a = 0.0f;
        for (int j = 0; j < IN; j++) a += rech_[c * IN + j] * layerIn[f * IN + j];
        x_[f * C + c] = a;
      }
    if (headIn) memcpy(h_, headIn, sizeof(float) * n * C);
    else memset(h_, 0, sizeof(float) * n * C);

    for (int i = 0; i < NL; i++) {
      Layer& L = L_[i];
      L.ring.write(x_, n);
      for (int f = 0; f < n; f++)
        for (int c = 0; c < C; c++) z_[f * C + c] = L.cb[c] + L.mix[c] * cond[f];
      for (int k = 0; k < K; k++) {
        const float* __restrict src = L.ring.at(n, (K - 1 - k) * L.D);
        const float* __restrict wk = &L.w[k * C * C];
        for (int f = 0; f < n; f++) {
          const float* __restrict s = src + f * C;
          float* __restrict z = &z_[f * C];
          for (int j = 0; j < C; j++) {
            const float sj = s[j];
            for (int c = 0; c < C; c++) z[c] += wk[j * C + c] * sj;
          }
        }
      }
      for (int f = 0; f < n; f++) {
        float* __restrict z = &z_[f * C];
        float* __restrict h = &h_[f * C];
        for (int c = 0; c < C; c++) { z[c] = actTanh<Fast>(z[c]); h[c] += z[c]; }
        float* __restrict x = &x_[f * C];
        for (int c = 0; c < C; c++) {
          float a = L.l1b[c];
          for (int b = 0; b < C; b++) a += L.l1w[b * C + c] * z[b];
          x[c] += a;
        }
      }
    }
    if (layerOut) memcpy(layerOut, x_, sizeof(float) * n * C);
    for (int f = 0; f < n; f++)
      for (int o = 0; o < HS; o++) {
        float a = headB_[o];
        for (int c = 0; c < C; c++) a += headW_[o * C + c] * h_[f * C + c];
        headOut[f * HS + o] = a;
      }
  }

private:
  struct Layer {
    int D = 0;
    float w[K * C * C];
    float cb[C], mix[C], l1w[C * C], l1b[C];
    Ring<C> ring;
  };
  Layer L_[NL];
  float rech_[C * IN];
  float headW_[HS * C];
  float headB_[HS];
  float x_[kMaxFrames * C];
  float h_[kMaxFrames * C];
  float z_[kMaxFrames * C];
};

// A1 WaveNet 2배열 (Nano: 4/2/2, Feather: 8/4/4). 배열0 헤드 출력 채널 = 배열1 채널.
template <int C0, int HS0, int C1>
class WaveNetA1 : public Model {
  static_assert(HS0 == C1, "배열0 head_size는 배열1 channels와 같아야 함");
public:
  WaveNetA1(const char* name, const char* cfg) : name_(name), cfg_(cfg) {}
  const char* name() const override { return name_; }
  const char* config() const override { return cfg_; }
  int macsPerSample() const override { return A0::macs() + A1::macs(); }
  int tanhPerSample() const override { return A0::tanhs() + A1::tanhs(); }

  bool build(Pool& pool) override {
    static const int d0[7] = {1, 2, 4, 8, 16, 32, 64};
    static const int d1[13] = {128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    Rng r;
    a0_.build(pool, r, d0, false);
    a1_.build(pool, r, d1, true);
    return !pool.failed();
  }
  void reset() override { a0_.reset(); a1_.reset(); }
  void process(const float* in, float* out, int n, bool fast) override {
    if (fast) run<true>(in, out, n); else run<false>(in, out, n);
  }

private:
  using A0 = A1Array<1, C0, HS0, 7>;
  using A1 = A1Array<C0, C1, 1, 13>;
  template <bool Fast>
  void run(const float* in, float* out, int n) {
    a0_.template process<Fast>(in, in, nullptr, x0_, h0_, n);
    a1_.template process<Fast>(x0_, in, h0_, nullptr, out, n);
    for (int f = 0; f < n; f++) out[f] *= 0.02f;  // head_scale
  }
  const char* name_;
  const char* cfg_;
  A0 a0_;
  A1 a1_;
  float x0_[kMaxFrames * C0];
  float h0_[kMaxFrames * HS0];
};

// ================================================================ LSTM
// NAM/lstm.cpp: gates = W·[x, h] + b (순서 i, f, g, o), c = σ(f)c + σ(i)tanh(g), h = σ(o)tanh(c), out = head·h + b
template <int H, int NL>
class Lstm : public Model {
public:
  Lstm(const char* name, const char* cfg) : name_(name), cfg_(cfg) {}
  const char* name() const override { return name_; }
  const char* config() const override { return cfg_; }
  int macsPerSample() const override {
    int m = 0;
    for (int l = 0; l < NL; l++) m += 4 * H * ((l == 0 ? 1 : H) + H);
    return m + H;
  }
  int tanhPerSample() const override { return NL * 5 * H; }  // tanh 2H + sigmoid 3H

  bool build(Pool&) override {
    Rng r;
    for (int l = 0; l < NL; l++) {
      Cell& c = cell_[l];
      c.I = (l == 0) ? 1 : H;
      r.fill(c.W, 4 * H * (c.I + H), 1.0f / sqrtf((float)(c.I + H)));
      r.fill(c.b, 4 * H, 0.1f);
    }
    r.fill(headW_, H, 0.3f);
    headB_ = 0.0f;
    reset();
    return true;
  }
  void reset() override {
    for (auto& c : cell_) { memset(c.xh, 0, sizeof(c.xh)); memset(c.c, 0, sizeof(c.c)); }
  }
  void process(const float* in, float* out, int n, bool fast) override {
    if (fast) run<true>(in, out, n); else run<false>(in, out, n);
  }

private:
  struct Cell {
    int I = 1;
    float W[4 * H * (H + H)];  // 행 우선 [4H][I+H]
    float b[4 * H];
    float xh[H + H];           // [x(I), h(H)]
    float c[H];
    float g[4 * H];
  };

  template <bool Fast>
  void run(const float* in, float* out, int n) {
    for (int f = 0; f < n; f++) {
      const float* x = &in[f];
      for (int l = 0; l < NL; l++) {
        Cell& c = cell_[l];
        const int I = c.I, W = I + H;
        for (int i = 0; i < I; i++) c.xh[i] = x[i];
        const float* __restrict xh = c.xh;
        float* __restrict g = c.g;
        for (int r = 0; r < 4 * H; r++) {
          const float* __restrict w = &c.W[r * W];
          float a = c.b[r];
          for (int j = 0; j < W; j++) a += w[j] * xh[j];
          g[r] = a;
        }
        for (int i = 0; i < H; i++)
          c.c[i] = actSigmoid<Fast>(c.g[H + i]) * c.c[i] + actSigmoid<Fast>(c.g[i]) * actTanh<Fast>(c.g[2 * H + i]);
        for (int i = 0; i < H; i++)
          c.xh[I + i] = actSigmoid<Fast>(c.g[3 * H + i]) * actTanh<Fast>(c.c[i]);
        x = &c.xh[I];  // 다음 층 입력 = 이 층 h
      }
      float y = headB_;
      for (int i = 0; i < H; i++) y += headW_[i] * x[i];
      out[f] = y;
    }
  }

  const char* name_;
  const char* cfg_;
  Cell cell_[NL];
  float headW_[H];
  float headB_ = 0.0f;
};

}  // namespace bench
