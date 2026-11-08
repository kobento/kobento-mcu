// The microphone of a Teensy Audio Shield (SGTL5000) wired to this board's expansion headers
// (see ../../README.md): for apps that stream it. board::displayInit()
// brings the codec up; these are false/0 when no shield answered.
#pragma once
#include <cstdint>

namespace board {

constexpr int kMicRate = 48000;

bool micReady();
// Read `frames` mono 16-bit samples (the left slot) at kMicRate, blocking up to timeoutMs.
// Returns the frames read.
int micRead(int16_t* out, int frames, int timeoutMs);
// The analog mic preamp: 0, 20, 30 or 40 dB (nearest lower step).
void micGain(int db);

}  // namespace board
