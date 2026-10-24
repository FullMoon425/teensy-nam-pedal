// 02_audio_passthrough: 오디오실드 LINE IN → LINE OUT 패스스루
// 기타(또는 폰 음악) → LINE IN, LINE OUT(또는 헤드폰) → 앰프/이어폰으로 소리가 그대로 나오면 통과.
//
// 주의: Teensy Audio Library 기본 샘플레이트는 44.1kHz입니다.
// 48kHz 설정은 06_nam_benchmark(S0)에서 검증합니다.

#include <Audio.h>

AudioInputI2S        audioIn;
AudioOutputI2S       audioOut;
AudioConnection      patchL(audioIn, 0, audioOut, 0);
AudioConnection      patchR(audioIn, 0, audioOut, 1);  // 모노 기타 입력을 양쪽 출력으로
AudioControlSGTL5000 codec;

// 코덱 레벨: 기타 소리가 찌그러지면 LINE_IN_LEVEL을 낮추세요(숫자가 작을수록 큰 입력 허용).
// lineInLevel  0 = 3.12Vpp ... 15 = 0.24Vpp
// lineOutLevel 13 = 3.16Vpp ... 31 = 1.16Vpp
#define LINE_IN_LEVEL   5
#define LINE_OUT_LEVEL  29
#define HEADPHONE_VOL   0.5f

void setup() {
  Serial.begin(115200);
  AudioMemory(12);

  codec.enable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(LINE_IN_LEVEL);
  codec.lineOutLevel(LINE_OUT_LEVEL);
  codec.volume(HEADPHONE_VOL);  // 헤드폰 잭으로 들을 때

  Serial.println("02_audio_passthrough 시작");
  Serial.printf("샘플레이트: %.1f Hz, 블록: %d 샘플\n", AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES);
}

void loop() {
  static elapsedMillis since;
  if (since >= 1000) {
    since = 0;
    Serial.printf("CPU %.1f%% (최대 %.1f%%), 오디오 메모리 %d (최대 %d)\n",
                  AudioProcessorUsage(), AudioProcessorUsageMax(),
                  AudioMemoryUsage(), AudioMemoryUsageMax());
  }
}
