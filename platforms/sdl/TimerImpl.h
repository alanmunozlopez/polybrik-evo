#ifndef PLATFORMS_SDL_TIMERIMPL_H_
#define PLATFORMS_SDL_TIMERIMPL_H_

#include "Platform.h"

// Construct after SDL_Init.
class TimerImpl final : public Timer
{
  public:
    TimerImpl();
    uint32_t millis() override;
    uint32_t micros() override;
    void delay(int ms) override;
    uint32_t clockCycles() override;
    uint32_t clockCyclesPerUs() override;
    // MIDI clock out (core/source/mtMidi.cpp sendMidiClock): timed by midi/MidiHub's thread, as the
    // IMX does with a hardware timer. Without a running hub (or with --sim-clock) the callback runs
    // at once, the Timer default.
    void scheduleMidiClockUs(uint32_t delayUs, void (*callback)()) override;
    void cancelMidiClock() override;

  private:
    uint64_t t0;
    uint64_t pc0;
};

#endif
