#ifndef PLATFORMS_SDL_MIDI_MIDIPARSER_H_
#define PLATFORMS_SDL_MIDI_MIDIPARSER_H_

#include <cstdint>

// One parsed MIDI message. Channel messages carry channel 1..16 (the firmware's convention, as
// platforms/imx/usb/usb_midi.c and the Arduino MIDI library hand it over); system messages carry 0.
struct MidiMessage
{
    uint8_t type = 0;    // status without the channel nibble (0x80..0xE0), or the full system status (0xF1..0xFF)
    uint8_t channel = 0; // 1..16 for channel messages, 0 for system messages
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// Byte stream to messages, as a raw MIDI port delivers them: running status, real-time bytes
// interleaved anywhere (also inside other messages), System Exclusive skipped whole.
// A Note On with velocity 0 is reported as a Note Off, as the Arduino MIDI library does by default.
class MidiParser
{
  public:
    // Feeds one byte; returns true and fills `out` when the byte completes a message.
    bool feed(uint8_t byte, MidiMessage& out);
    void reset();

  private:
    uint8_t status = 0;  // running status (0 = none)
    uint8_t data[2] = {};
    uint8_t count = 0;   // data bytes received for the current status
    bool inSysex = false;
};

// Message to bytes. Returns the length (1..3). Channel is 1..16; out of range channels wrap like
// the hardware drivers do ((channel - 1) & 0x0F).
int midiEncode(uint8_t type, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t out[3]);

// Data bytes that follow a status byte (0..2), -1 for System Exclusive and undefined statuses.
int midiDataLength(uint8_t status);

#endif
