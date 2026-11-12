#ifndef PLATFORMS_SDL_MIDI_UARTMIDIIMPL_H_
#define PLATFORMS_SDL_MIDI_UARTMIDIIMPL_H_

#include "Platform.h"

// The firmware's MIDI "Jack" interface, on the MidiHub Jack bus: USB host MIDI devices whose name
// matches a line of <sd>/midi-jack (a USB to DIN cable, typically). Mirrors
// platforms/imx/source/UartMidiImpl.cpp, which drives the Arduino MIDI library: channels 1..16,
// one handler per message type. Actions are copied for the same reason as UsbMidiImpl.
class UartMidiImpl final : public UartMidi
{
  public:
    bool isReadAvailable() override;
    void read() override;
    void sendRealTime(MessageType messageType) override;
    void sendControlChange(uint8_t index, uint8_t value, uint8_t channel) override;
    void sendProgramChange(uint8_t programNumber, uint8_t channel) override;
    void sendAfterTouch(uint8_t velocity, uint8_t channel) override;
    void sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) override;
    void sendPitchBend(int value, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void init() override;
    void setReceiveActions(MidiReceiveActions* receiveActions) override;
    void setReceiveNoteOnAction(void (*action)(uint8_t, uint8_t, uint8_t)) override;

  private:
    MidiReceiveActions receiveActions = {};
};

#endif
