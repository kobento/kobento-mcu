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
// The monitor: the headphones on the shield's jack play what micMonitorWrite gets (the app's
// processed capture), ~25 ms later. 0 is off (headphones muted), 1..100 is -40..0 dB.
void micMonitor(int percent);
void micMonitorWrite(const int16_t* mono, int frames);
// Tests: the I2S counters (output buffers played with nothing new, input buffers lost,
// monitor writes that did not fit), and the codec looping its I2S input back to its output.
void micAudioCounts(int* txStarved, int* rxLost, int* txShort);
void micDigitalLoop(bool on);
void micWatchRight(bool on, int* nonzero, int* maxAbs);
void micRightLog();   // a test: the right slot read

}  // namespace board
