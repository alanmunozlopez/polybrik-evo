// Self-check of the raw MIDI host: the parser and encoder, then MidiHub with UsbMidiImpl and
// UartMidiImpl over two pseudo-terminals in raw mode, which behave like /dev/snd/midiC*D* (a byte
// stream each way). POLYBRIK_MIDI_DEV hands the pty paths to the hub; midi-jack sends the second
// one to the Jack bus. Covers input dispatch, immediate output, output held to the audio block's
// play time, the scheduled MIDI clock, cancel, and unplug.
#undef NDEBUG
#include "midi/MidiHub.h"
#include "midi/MidiParser.h"
#include "midi/UartMidiImpl.h"
#include "midi/UsbMidiImpl.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::vector<MidiMessage> parse(const std::vector<uint8_t>& bytes)
{
    MidiParser p;
    std::vector<MidiMessage> out;
    for (uint8_t b : bytes)
    {
        MidiMessage m;
        if (p.feed(b, m))
            out.push_back(m);
    }
    return out;
}

bool same(const MidiMessage& m, uint8_t type, uint8_t ch, uint8_t d1, uint8_t d2)
{
    return m.type == type && m.channel == ch && m.data1 == d1 && m.data2 == d2;
}

void testParser()
{
    // Running status, Note On velocity 0 as Note Off, real-time inside a message, SysEx skipped.
    auto m = parse({ 0x90, 60, 100, 62, 0, 0xF8, 0xF0, 1, 2, 3, 0xF7, 0xB3, 7, 0xFA, 127, 0xC0, 5, 0xE0, 0, 64,
                     0xF2, 0x10, 0x01, 0xFE });
    assert(m.size() == 9);
    assert(same(m[0], 0x90, 1, 60, 100));
    assert(same(m[1], 0x80, 1, 62, 0));
    assert(same(m[2], 0xF8, 0, 0, 0));
    assert(same(m[3], 0xFA, 0, 0, 0)); // the real-time byte comes out before the CC it interrupted
    assert(same(m[4], 0xB0, 4, 7, 127));
    assert(same(m[5], 0xC0, 1, 5, 0));
    assert(same(m[6], 0xE0, 1, 0, 64));
    assert(same(m[7], 0xF2, 0, 0x10, 0x01));
    assert(same(m[8], 0xFE, 0, 0, 0));
    // Data with no status, and data after SysEx, are ignored; system common clears running status.
    assert(parse({ 60, 100, 0xF0, 0x90, 0xF7, 60, 100 }).empty());
    assert(parse({ 0x90, 60, 100, 0xF6, 61, 100 }).size() == 2);

    uint8_t b[3];
    assert(midiEncode(0x90, 1, 60, 100, b) == 3 && b[0] == 0x90 && b[1] == 60 && b[2] == 100);
    assert(midiEncode(0x80, 16, 60, 0, b) == 3 && b[0] == 0x8F);
    assert(midiEncode(0xC0, 3, 200, 0, b) == 2 && b[0] == 0xC2 && b[1] == 72); // data masked to 7 bits
    assert(midiEncode(0xF8, 0, 0, 0, b) == 1 && b[0] == 0xF8);

    assert(MidiHub::matchesAny("mio: mio MIDI 1", { "MIO" }));
    assert(!MidiHub::matchesAny("Arturia MiniLab", { "mio", "" }));
    puts("parser ok");
}

struct Pty
{
    int master = -1;
    int slaveFd = -1; // kept open: macOS resets a pty's termios when its last slave fd closes
    std::string slave;
};

Pty openPty()
{
    Pty p;
    p.master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(p.master >= 0);
    assert(grantpt(p.master) == 0 && unlockpt(p.master) == 0);
    p.slave = ptsname(p.master);
    // Raw both ways: no echo, no CR/LF translation, byte-at-a-time reads.
    int s = open(p.slave.c_str(), O_RDWR | O_NOCTTY);
    assert(s >= 0);
    termios t{};
    assert(tcgetattr(s, &t) == 0);
    cfmakeraw(&t);
    assert(tcsetattr(s, TCSANOW, &t) == 0);
    p.slaveFd = s;
    assert(tcgetattr(p.master, &t) == 0);
    cfmakeraw(&t);
    tcsetattr(p.master, TCSANOW, &t);
    fcntl(p.master, F_SETFL, O_NONBLOCK);
    return p;
}

void put(const Pty& p, std::vector<uint8_t> bytes)
{
    assert(write(p.master, bytes.data(), bytes.size()) == (ssize_t)bytes.size());
}

// Reads what the hub wrote, waiting up to `ms` for `n` bytes.
std::vector<uint8_t> take(const Pty& p, size_t n, int ms)
{
    std::vector<uint8_t> got;
    for (int i = 0; i < ms && got.size() < n; i++)
    {
        uint8_t buf[64];
        ssize_t r = read(p.master, buf, sizeof buf);
        if (r > 0)
            got.insert(got.end(), buf, buf + r);
        else
            sleepMs(1);
    }
    return got;
}

template <class F> bool waitFor(F cond, int ms)
{
    for (int i = 0; i < ms; i++)
    {
        if (cond())
            return true;
        sleepMs(1);
    }
    return cond();
}

// Receive handlers: plain functions, as the firmware's are.
std::vector<MidiMessage> g_usbIn, g_jackIn;
void usbRealTime(uint8_t t) { g_usbIn.push_back({ t, 0, 0, 0 }); }
void usbNoteOn(uint8_t c, uint8_t n, uint8_t v) { g_usbIn.push_back({ 0x90, c, n, v }); }
void usbNoteOff(uint8_t c, uint8_t n, uint8_t v) { g_usbIn.push_back({ 0x80, c, n, v }); }
void usbSongPos(uint16_t b) { g_usbIn.push_back({ 0xF2, 0, (uint8_t)(b & 0x7F), (uint8_t)(b >> 7) }); }
void usbCc(uint8_t c, uint8_t n, uint8_t v) { g_usbIn.push_back({ 0xB0, c, n, v }); }
void jackNoteOn(uint8_t c, uint8_t n, uint8_t v) { g_jackIn.push_back({ 0x90, c, n, v }); }
void jackNoteOff(uint8_t c, uint8_t n, uint8_t v) { g_jackIn.push_back({ 0x80, c, n, v }); }
void jackClock() { g_jackIn.push_back({ 0xF8, 0, 0, 0 }); }
void jackStart() { g_jackIn.push_back({ 0xFA, 0, 0, 0 }); }
void jackStop() { g_jackIn.push_back({ 0xFC, 0, 0, 0 }); }
void jackContinue() { g_jackIn.push_back({ 0xFB, 0, 0, 0 }); }
void jackSongPos(unsigned b) { g_jackIn.push_back({ 0xF2, 0, (uint8_t)(b & 0x7F), (uint8_t)(b >> 7) }); }
void jackCc(uint8_t c, uint8_t n, uint8_t v) { g_jackIn.push_back({ 0xB0, c, n, v }); }

std::atomic<int> g_clockFired{ 0 };
std::atomic<int64_t> g_clockAt{ 0 };
UsbMidiImpl* g_usb = nullptr;
void clockCallback() // as sendMidiClockImmediate: runs on the hub thread and sends through the impl
{
    g_clockAt = MidiHub::nowUs();
    g_clockFired++;
    g_usb->sendRealTime(Midi::kClock);
}

void testHub()
{
    char tmpl[] = "/tmp/midi_test_XXXXXX";
    assert(mkdtemp(tmpl));
    const std::string sd = tmpl;
    Pty usbPort = openPty(), jackPort = openPty();
    FILE* f = fopen((sd + "/midi-jack").c_str(), "w");
    assert(f);
    fprintf(f, "# DIN cable\n%s\n", jackPort.slave.c_str());
    fclose(f);
    setenv("POLYBRIK_MIDI_DEV", (usbPort.slave + ":" + jackPort.slave).c_str(), 1);

    UsbMidiImpl usb;
    UartMidiImpl jack;
    g_usb = &usb;
    // Messages before setReceiveActions must not crash (null handlers).
    UsbMidi::MidiReceiveActions ua{ usbRealTime, usbNoteOn, usbNoteOff, usbSongPos, usbCc };
    UartMidi::MidiReceiveActions ja{ jackNoteOn, jackNoteOff, jackClock, jackStart, jackStop, jackSongPos, jackContinue, jackCc };
    usb.setReceiveActions(&ua);
    jack.setReceiveActions(&ja);
    jack.init();

    MidiHub& hub = MidiHub::instance();
    assert(!hub.running());
    hub.start(sd.c_str());
    assert(waitFor([&] { return hub.hasOutput(MidiHub::kUsb) && hub.hasOutput(MidiHub::kJack); }, 3000));

    // ---- input ----
    put(usbPort, { 0x90, 60, 100, 62, 0, 0xF8, 0xB1, 7, 127, 0xF2, 0x10, 0x01 });
    put(jackPort, { 0xFA, 0xF8, 0x95, 40, 1, 0xFC, 0xFB });
    auto drain = [&] {
        while (usb.isReadAvailable()) usb.read();
        while (jack.isReadAvailable()) jack.read();
    };
    const bool got = waitFor([&] { drain(); return g_usbIn.size() >= 5 && g_jackIn.size() >= 5; }, 2000);
    if (!got)
        fprintf(stderr, "input: usb %zu, jack %zu messages\n", g_usbIn.size(), g_jackIn.size());
    assert(got);
    assert(g_usbIn.size() == 5 && g_jackIn.size() == 5);
    assert(same(g_usbIn[0], 0x90, 1, 60, 100));
    assert(same(g_usbIn[1], 0x80, 1, 62, 0));
    assert(same(g_usbIn[2], 0xF8, 0, 0, 0));
    assert(same(g_usbIn[3], 0xB0, 2, 7, 127));
    assert(same(g_usbIn[4], 0xF2, 0, 0x10, 0x01));
    assert(same(g_jackIn[0], 0xFA, 0, 0, 0));
    assert(same(g_jackIn[1], 0xF8, 0, 0, 0));
    assert(same(g_jackIn[2], 0x90, 6, 40, 1));
    assert(same(g_jackIn[3], 0xFC, 0, 0, 0));
    assert(same(g_jackIn[4], 0xFB, 0, 0, 0));
    puts("input ok");

    // Note On handler swap (setHandleNoteOnAction / setReceiveNoteOnAction).
    jack.setReceiveNoteOnAction(usbNoteOn);
    put(jackPort, { 0x90, 1, 2 });
    size_t before = g_usbIn.size();
    assert(waitFor([&] { drain(); return g_usbIn.size() == before + 1; }, 2000));
    assert(same(g_usbIn.back(), 0x90, 1, 1, 2));
    jack.setReceiveNoteOnAction(jackNoteOn);

    // ---- immediate output (not inside an audio block) ----
    usb.sendNoteOn(60, 100, 1);
    usb.sendControlChange(74, 64, 10);
    jack.sendPitchBend(0, 2);
    jack.sendProgramChange(9, 16);
    jack.sendRealTime(Midi::kStart);
    auto u = take(usbPort, 6, 1000);
    assert((u == std::vector<uint8_t>{ 0x90, 60, 100, 0xB9, 74, 64 }));
    auto j = take(jackPort, 6, 1000);
    assert((j == std::vector<uint8_t>{ 0xE1, 0x00, 0x40, 0xCF, 9, 0xFA }));
    puts("output ok");

    // ---- output inside an audio block waits for the block's play time ----
    int64_t t0 = MidiHub::nowUs();
    MidiHub::beginAudioBlock(t0 + 60000);
    usb.sendNoteOff(60, 0, 1);
    MidiHub::endAudioBlock();
    assert(take(usbPort, 3, 25).empty());
    u = take(usbPort, 3, 500);
    int64_t late = MidiHub::nowUs() - (t0 + 60000);
    assert((u == std::vector<uint8_t>{ 0x80, 60, 0 }));
    assert(late > -5000 && late < 30000);
    printf("timed output ok (%lld us after the block's play time)\n", (long long)late);

    // ---- MIDI clock: scheduled from the block, fired on the hub thread ----
    t0 = MidiHub::nowUs();
    MidiHub::beginAudioBlock(t0 + 40000);
    hub.scheduleClock(1000, clockCallback); // 1 ms into the block
    MidiHub::endAudioBlock();
    assert(waitFor([] { return g_clockFired.load() == 1; }, 500));
    const int64_t err = g_clockAt.load() - (t0 + 41000);
    assert(err > -3000 && err < 30000);
    u = take(usbPort, 1, 500);
    assert((u == std::vector<uint8_t>{ 0xF8 }));
    printf("clock ok (%lld us late)\n", (long long)err);

    // Cancel drops pending clocks but keeps pending notes.
    t0 = MidiHub::nowUs();
    MidiHub::beginAudioBlock(t0 + 80000);
    hub.scheduleClock(0, clockCallback);
    usb.sendNoteOn(1, 1, 1);
    MidiHub::endAudioBlock();
    hub.cancelClock();
    u = take(usbPort, 3, 400);
    assert((u == std::vector<uint8_t>{ 0x90, 1, 1 }));
    sleepMs(50);
    assert(g_clockFired.load() == 1);
    puts("cancel ok");

    // ---- All Notes Off on every output ----
    hub.allNotesOff();
    u = take(usbPort, 48, 1000);
    assert(u.size() == 48 && u[0] == 0xB0 && u[1] == 123 && u[45] == 0xBF);

    // ---- unplug: the hub closes the port and the bus has no output any more ----
    close(usbPort.slaveFd);
    close(usbPort.master);
    if (waitFor([&] { usb.sendNoteOn(1, 1, 1); return !hub.hasOutput(MidiHub::kUsb); }, 3000))
        puts("unplug ok");
    else
        puts("unplug: pty hang-up not reported on this host (skipped)");
    assert(hub.hasOutput(MidiHub::kJack));
}
} // namespace

int main()
{
    testParser();
    testHub();
    puts("midi_test: all ok");
    return 0;
}
