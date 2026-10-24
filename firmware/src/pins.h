#pragma once
// Teensy 4.1 핀 배치. 핀을 바꾸면 docs/hardware-design.md 5장과 sketches/의 #define도 같이 고칠 것.
// 오디오실드 Rev D 사용 핀(피할 것): I2S 7, 8, 20, 21, 23 / I2C 18, 19
// 상태: 초안 (H2에서 확정)

// 작은 노브 (B10K, 3.3V 기준)
constexpr int PIN_POT1 = 14;  // A0
constexpr int PIN_POT2 = 15;  // A1
constexpr int PIN_POT3 = 16;  // A2

// 큰 노브 (EC11 인코더, 내부 풀업)
constexpr int PIN_ENC_A  = 2;
constexpr int PIN_ENC_B  = 3;
constexpr int PIN_ENC_SW = 4;

// 풋스위치 (모멘터리, 내부 풀업)
constexpr int PIN_FOOTSW = 5;

// 바이패스 LED
constexpr int PIN_LED = 9;

// OLED SPI (MOSI 11, SCK 13은 하드웨어 SPI 고정)
constexpr int PIN_OLED_CS  = 10;
constexpr int PIN_OLED_DC  = 24;
constexpr int PIN_OLED_RST = 25;

// (확장) 릴레이 트루 바이패스
constexpr int PIN_RELAY = 28;
