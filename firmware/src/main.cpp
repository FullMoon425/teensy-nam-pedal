// teensy-nam-pedal 메인 펌웨어
// 지금은 패스스루 뼈대입니다. software-design.md 10장의 S2~S5 순서로 모듈을 채워 나갑니다.
//   S2: audio/AudioEffectNAM 을 패스스루 사이에 끼워 모델 재생
//   S3: storage/ModelLoader, Presets
//   S4: ui/Inputs, Pages, Display

#include <Arduino.h>
#include <Audio.h>
#include "pins.h"

AudioInputI2S        audioIn;
AudioOutputI2S       audioOut;
AudioConnection      patchL(audioIn, 0, audioOut, 0);
AudioConnection      patchR(audioIn, 0, audioOut, 1);
AudioControlSGTL5000 codec;

void setup() {
  Serial.begin(115200);
  AudioMemory(16);

  codec.enable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(0);   // 3.12Vpp. 02 패스스루 측정값으로 확정 후 변경 (hardware-design.md 3-1절)
  codec.lineOutLevel(29);

  pinMode(PIN_LED, OUTPUT);
}

void loop() {
  static elapsedMillis since;
  if (since >= 1000) {
    since = 0;
    Serial.printf("CPU %.1f%% (max %.1f%%)\n", AudioProcessorUsage(), AudioProcessorUsageMax());
  }
}
