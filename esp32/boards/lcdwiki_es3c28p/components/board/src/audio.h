// Inside the board component: the Teensy Audio Shield (SGTL5000) driver in audio.cpp.
#pragma once
#include <cstdint>
#include <string>

#include "driver/i2c_master.h"

namespace board {

i2c_master_dev_handle_t i2cDevice(uint8_t addr);   // board.cpp: the shared bus (IO16/IO15)

void audioInit();          // I2S first (the codec needs MCLK for I2C), then the codec
bool audioReady();
void audioInput(bool mic);   // true: the mic pads, false: line in
bool audioLevel(int ms, float* peak, float* rms);
bool audioConsole(const std::string& line);   // "audio", "tone", "volume", "mic", "line", "rec", "wav", "play", "loop", "spk"

// speaker.cpp: the board's ES8311 and amplifier.
void speakerInit();
bool speakerReady();
void speakerVolume(float v);   // 0..1
void speakerClock(bool on);
void speakerWrite(const int16_t* stereo, int frames);   // blocks until queued

}  // namespace board
