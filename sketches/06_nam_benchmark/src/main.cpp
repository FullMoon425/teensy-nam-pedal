// 06_nam_benchmark (S0) 뼈대: 48kHz 설정 검증
// LINE OUT L로 1kHz 사인을 내보내고 시리얼에 샘플레이트와 CPU를 찍습니다.
// 주파수가 1000Hz면 정상, 약 1088Hz 또는 919Hz면 설정 불일치 (docs/software-design.md 3장).
// 아키텍처별 벤치마크 AudioStream 객체는 S0 프롬프트(software-design.md 11장)로 만들어 여기에 추가합니다.

#include <Arduino.h>
#include <Audio.h>

// SGTL5000 enable()은 CHIP_CLK_CTRL에 44.1kHz(0x0004)를 쓰므로 48kHz(0x0008)로 덮어씀.
// write()가 protected라 상속해서 호출합니다.
class Codec48k : public AudioControlSGTL5000 {
public:
  bool setSysFs48k() { return write(0x0004, 0x0008); }  // SYS_FS=48kHz, MCLK=256*Fs
};

// 1 = LINE OUT L에 1kHz 사인(주파수 측정), 0 = LINE IN L 패스스루(02와 같은 소리 확인)
#define TEST_SINE  1

AudioSynthWaveformSine sine;
AudioInputI2S          audioIn;
AudioOutputI2S         audioOut;
#if TEST_SINE
AudioConnection        patchL(sine, 0, audioOut, 0);
#else
AudioConnection        patchL(audioIn, 0, audioOut, 0);
#endif
AudioConnection        patchR(audioIn, 0, audioOut, 1);
Codec48k               codec;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  AudioMemory(12);

  codec.enable();
  bool ok = codec.setSysFs48k();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(0);   // 3.12Vpp (hardware-design.md 3-1절)
  codec.lineOutLevel(29);

  sine.frequency(1000);
  sine.amplitude(0.5f);

  Serial.println("06_nam_benchmark 시작");
  Serial.printf("AUDIO_SAMPLE_RATE_EXACT = %.1f Hz, 블록 %d 샘플, SYS_FS 48k 설정 %s\n",
                AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES, ok ? "성공" : "실패");
}

void loop() {
  static elapsedMillis since;
  if (since >= 1000) {
    since = 0;
    Serial.printf("CPU %.1f%% (최대 %.1f%%), 오디오 메모리 최대 %d\n",
                  AudioProcessorUsage(), AudioProcessorUsageMax(), AudioMemoryUsageMax());
  }
}
