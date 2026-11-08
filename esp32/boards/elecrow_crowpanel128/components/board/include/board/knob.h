// The CrowPanel rotary's extra input and output, beyond board/board.h: the knob (a quadrature
// encoder around the screen), its push switch (pressing the whole display), and the ring of
// RGB LEDs behind it. Only boards with a knob have this header; apps test for it with
// __has_include("board/knob.h").
#pragma once
#include <cstdint>

namespace board {

// Detents turned since boot: + clockwise, - counter-clockwise. Any task.
int knobPosition();
// The push switch, debounced: true while held. Any task.
bool knobDown();
// Presses of the switch since boot, counted as they happen (sampled every 5 ms): short ones
// (released before 0.8 s) and long ones (counted when they reach 0.8 s, still held).
int knobShortPresses();
int knobLongPresses();

// The LED ring, clockwise from the top as seen from the front.
constexpr int kLedCount = 5;
// Set every LED (0xRRGGBB each, n <= kLedCount) and send them. Unchanged colours aren't sent
// again. Any task.
void setLeds(const uint32_t* rgb, int n);
// The red power LED beside the USB port (GPIO40, active low).
void setPowerLed(bool on);

}  // namespace board
