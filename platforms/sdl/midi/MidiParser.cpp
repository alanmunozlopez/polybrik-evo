#include "midi/MidiParser.h"

int midiDataLength(uint8_t status)
{
    if (status < 0x80)
        return -1;
    if (status < 0xF0)
    {
        switch (status & 0xF0)
        {
            case 0xC0: // program change
            case 0xD0: // channel pressure
                return 1;
            default: return 2;
        }
    }
    switch (status)
    {
        case 0xF1: // MTC quarter frame
        case 0xF3: // song select
            return 1;
        case 0xF2: // song position
            return 2;
        case 0xF6: // tune request
        case 0xF8:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFE:
        case 0xFF:
            return 0;
        default: return -1; // F0 / F7 (SysEx), F4, F5, F9, FD
    }
}

void MidiParser::reset()
{
    status = 0;
    count = 0;
    inSysex = false;
}

bool MidiParser::feed(uint8_t byte, MidiMessage& out)
{
    if (byte >= 0xF8)
    {
        // Real-time: never touches running status, may arrive inside any other message.
        if (midiDataLength(byte) != 0)
            return false; // F9, FD: undefined
        out = MidiMessage{ byte, 0, 0, 0 };
        return true;
    }
    if (byte >= 0x80)
    {
        if (byte == 0xF0)
        {
            inSysex = true;
            status = 0;
            return false;
        }
        inSysex = false; // any status byte ends a SysEx
        count = 0;
        if (byte == 0xF7)
        {
            status = 0; // end of SysEx is system common: no running status after it
            return false;
        }
        const int len = midiDataLength(byte);
        if (len < 0)
        {
            status = 0;
            return false;
        }
        if (len == 0)
        {
            status = 0; // system common clears running status
            out = MidiMessage{ byte, 0, 0, 0 };
            return true;
        }
        status = byte;
        return false;
    }

    // Data byte.
    if (inSysex || status == 0)
        return false;
    data[count++] = byte;
    const int len = midiDataLength(status);
    if (count < len)
        return false;
    count = 0;

    MidiMessage m;
    if (status < 0xF0)
    {
        m.type = status & 0xF0;
        m.channel = (uint8_t)((status & 0x0F) + 1);
    }
    else
    {
        m.type = status;
        m.channel = 0;
        status = 0; // system common has no running status
    }
    m.data1 = data[0];
    m.data2 = len > 1 ? data[1] : 0;
    if (m.type == 0x90 && m.data2 == 0)
        m.type = 0x80;
    out = m;
    return true;
}

int midiEncode(uint8_t type, uint8_t channel, uint8_t data1, uint8_t data2, uint8_t out[3])
{
    uint8_t status = type;
    if (type < 0xF0)
        status = (uint8_t)((type & 0xF0) | ((channel - 1) & 0x0F));
    const int len = midiDataLength(status);
    out[0] = status;
    if (len <= 0)
        return 1;
    out[1] = data1 & 0x7F;
    if (len == 1)
        return 2;
    out[2] = data2 & 0x7F;
    return 3;
}
