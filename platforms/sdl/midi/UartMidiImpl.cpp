#include "midi/UartMidiImpl.h"

#include "midi/MidiHub.h"

namespace
{
constexpr MidiHub::Bus kBus = MidiHub::kJack;

void out(uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2)
{
    MidiHub& hub = MidiHub::instance();
    if (!hub.hasOutput(kBus))
        return;
    uint8_t b[3];
    hub.send(kBus, b, midiEncode(type, channel, d1, d2, b));
}
} // namespace

// The firmware calls this at boot (PlatformImpl::init) and from resetMidi(); the hub is started by
// main, so there is no port to set up here.
void UartMidiImpl::init() {}

bool UartMidiImpl::isReadAvailable()
{
    return MidiHub::instance().inputPending(kBus);
}

// The Arduino MIDI library's per-type handlers, as UartMidiImpl::setReceiveActions wires them on the IMX.
void UartMidiImpl::read()
{
    MidiMessage m;
    if (!MidiHub::instance().pop(kBus, m))
        return;
    const MidiReceiveActions& a = receiveActions;
    switch (m.type)
    {
        case kNoteOn:
            if (a.noteOn) a.noteOn(m.channel, m.data1, m.data2);
            break;
        case kNoteOff:
            if (a.noteOff) a.noteOff(m.channel, m.data1, m.data2);
            break;
        case kControlChange:
            if (a.controlChange) a.controlChange(m.channel, m.data1, m.data2);
            break;
        case kClock:
            if (a.clock) a.clock();
            break;
        case kStart:
            if (a.start) a.start();
            break;
        case kStop:
            if (a.stop) a.stop();
            break;
        case kContinue:
            if (a.continueMessage) a.continueMessage();
            break;
        case kSongPosition:
            if (a.songPosition) a.songPosition((unsigned)((m.data1 & 0x7f) | ((m.data2 & 0x7f) << 7)));
            break;
        default: break;
    }
}

void UartMidiImpl::sendRealTime(MessageType messageType)
{
    if (messageType >= 0xF8)
        out(messageType, 0, 0, 0);
}

void UartMidiImpl::sendControlChange(uint8_t index, uint8_t value, uint8_t channel) { out(0xB0, channel, index, value); }
void UartMidiImpl::sendProgramChange(uint8_t programNumber, uint8_t channel) { out(0xC0, channel, programNumber, 0); }
void UartMidiImpl::sendAfterTouch(uint8_t velocity, uint8_t channel) { out(0xD0, channel, velocity, 0); }
void UartMidiImpl::sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) { out(0xA0, channel, note, value); }

// Arduino MIDI sendPitchBend(int): -8192..8191, centred on 0.
void UartMidiImpl::sendPitchBend(int value, uint8_t channel)
{
    value = value < -8192 ? -8192 : (value > 8191 ? 8191 : value);
    value += 8192;
    out(0xE0, channel, (uint8_t)(value & 0x7F), (uint8_t)((value >> 7) & 0x7F));
}

void UartMidiImpl::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) { out(0x90, channel, note, velocity); }
void UartMidiImpl::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) { out(0x80, channel, note, velocity); }

void UartMidiImpl::setReceiveActions(MidiReceiveActions* actions)
{
    if (actions)
        receiveActions = *actions;
}

void UartMidiImpl::setReceiveNoteOnAction(void (*action)(uint8_t, uint8_t, uint8_t))
{
    receiveActions.noteOn = action;
}
