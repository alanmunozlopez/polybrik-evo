#include "midi/UsbMidiImpl.h"

#include "midi/MidiHub.h"

namespace
{
constexpr MidiHub::Bus kBus = MidiHub::kUsb;

void out(uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2)
{
    MidiHub& hub = MidiHub::instance();
    if (!hub.hasOutput(kBus))
        return;
    uint8_t b[3];
    hub.send(kBus, b, midiEncode(type, channel, d1, d2, b));
}
} // namespace

bool UsbMidiImpl::isReadAvailable()
{
    return MidiHub::instance().inputPending(kBus);
}

// Same dispatch as platforms/imx/source/UsbMidiImpl.cpp.
void UsbMidiImpl::read()
{
    MidiMessage m;
    if (!MidiHub::instance().pop(kBus, m))
        return;
    switch (m.type)
    {
        case kClock:
        case kStart:
        case kStop:
        case kContinue:
            if (receiveActions.realTime)
                receiveActions.realTime(m.type);
            break;
        case kNoteOff:
            if (receiveActions.noteOff)
                receiveActions.noteOff(m.channel, m.data1, m.data2);
            break;
        case kNoteOn:
            if (receiveActions.noteOn)
                receiveActions.noteOn(m.channel, m.data1, m.data2);
            break;
        case kControlChange:
            if (receiveActions.controlChange)
                receiveActions.controlChange(m.channel, m.data1, m.data2);
            break;
        case kSongPosition:
            if (receiveActions.songPosition)
                receiveActions.songPosition((uint16_t)((m.data1 & 0x7f) | ((m.data2 & 0x7f) << 7)));
            break;
        default: break;
    }
}

void UsbMidiImpl::sendRealTime(MessageType messageType)
{
    switch (messageType)
    {
        case kClock:
        case kStart:
        case kContinue:
        case kStop:
        case kActiveSensing:
        case kSystemReset:
            out(messageType, 0, 0, 0);
            break;
        default: break;
    }
}

void UsbMidiImpl::sendControlChange(uint8_t index, uint8_t value, uint8_t channel) { out(0xB0, channel, index, value); }
void UsbMidiImpl::sendProgramChange(uint8_t programNumber, uint8_t channel) { out(0xC0, channel, programNumber, 0); }
void UsbMidiImpl::sendAfterTouch(uint8_t velocity, uint8_t channel) { out(0xD0, channel, velocity, 0); }
void UsbMidiImpl::sendPolyPressure(uint8_t note, uint8_t value, uint8_t channel) { out(0xA0, channel, note, value); }

void UsbMidiImpl::sendPitchBend(int value, uint8_t channel)
{
    value = value < -8192 ? -8192 : (value > 8191 ? 8191 : value);
    value += 8192;
    out(0xE0, channel, (uint8_t)(value & 0x7F), (uint8_t)((value >> 7) & 0x7F));
}

void UsbMidiImpl::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) { out(0x90, channel, note, velocity); }
void UsbMidiImpl::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) { out(0x80, channel, note, velocity); }

void UsbMidiImpl::setReceiveActions(MidiReceiveActions* actions)
{
    if (actions)
        receiveActions = *actions;
}
