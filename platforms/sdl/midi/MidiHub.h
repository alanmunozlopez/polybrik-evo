#ifndef PLATFORMS_SDL_MIDI_MIDIHUB_H_
#define PLATFORMS_SDL_MIDI_MIDIHUB_H_

#include "midi/MidiParser.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Linux raw MIDI for the firmware's two MIDI interfaces.
//
// The Tracker has a USB MIDI device port and a DIN/TRS jack. On the Brick both become USB host
// ports: every class-compliant device the kernel exposes as /dev/snd/midiC<card>D<dev> (snd-usb-audio)
// is opened, read and written here. Devices go to the firmware's "USB" interface unless their name
// matches a line of <sd>/midi-jack, which sends them to the "Jack" interface instead (for example a
// USB to DIN cable you want to treat as the Tracker's MIDI jack). Inputs of one bus are merged,
// outputs of one bus are sent to every device on it.
//
// One background thread owns the devices: it rescans /dev/snd once a second (hot-plug), reads and
// parses input into a queue per bus that the firmware drains in midiUpdate() (audio thread), and
// sends timed output.
//
// Timing. The firmware runs the sequencer inside the audio render, and SDL renders a whole device
// period (4 blocks on the Brick) ahead of when it is heard. Output sent while a block renders
// (sequencer notes, the transport, the MIDI clock scheduled with sub-block offsets) is therefore
// held back until that block is heard: the audio output tells the hub the play time of every block
// it renders (beginAudioBlock). Output from any other thread (UI previews) goes out at once.
// POLYBRIK_MIDI_OFFSET_MS (float, may be negative) shifts the timed output, for gear with its own latency.
//
// Extra ports for testing: POLYBRIK_MIDI_DEV="path[:path...]" opens those files too (a FIFO works),
// on the USB bus, or on the Jack bus when the path matches midi-jack.
class MidiHub
{
  public:
    enum Bus : uint8_t
    {
        kUsb = 0,
        kJack = 1,
        kBusCount = 2,
    };

    static MidiHub& instance();

    // Starts the device thread once. sdRoot holds the optional midi-jack file; may be null.
    void start(const char* sdRoot);
    bool running() const { return running_.load(); }

    // Input, drained by the firmware.
    bool inputPending(Bus bus) const { return inCount_[bus].load(std::memory_order_acquire) > 0; }
    bool pop(Bus bus, MidiMessage& out);

    // Output. Cheap no-op while no device on that bus can be written.
    bool hasOutput(Bus bus) const { return outAvail_[bus].load(std::memory_order_relaxed); }
    void send(Bus bus, const uint8_t* bytes, int len);

    // Timer::scheduleMidiClockUs / cancelMidiClock. Runs `callback` on the device thread at the time
    // the firmware asked for, shifted to the audible time when called from inside a block render.
    void scheduleClock(uint32_t delayUs, void (*callback)());
    void cancelClock();

    // CC 123 (All Notes Off) on all 16 channels of every output. Registered with atexit.
    void allNotesOff();

    // Audio timeline, called by the audio output around each rendered block, on the audio thread.
    static void beginAudioBlock(int64_t playUs);
    static void endAudioBlock();

    static int64_t nowUs(); // steady clock, the time base of every timestamp here

    // Name matching used for midi-jack (case-insensitive substring, '#' lines and blanks ignored).
    static bool matchesAny(const std::string& name, const std::vector<std::string>& patterns);

  private:
    MidiHub() = default;

    struct Device
    {
        std::string path;
        std::string name;
        int fd = -1;
        bool canRead = false;
        bool canWrite = false;
        Bus bus = kUsb;
        MidiParser parser;
        std::atomic<bool> dead{ false };
    };

    struct Event
    {
        Bus bus = kUsb;
        uint8_t bytes[3] = {};
        uint8_t len = 0;
        void (*callback)() = nullptr; // clock callback instead of bytes
    };

    void threadMain();
    void rescan();
    void openDevice(const std::string& path, const std::string& name);
    void closeDead();
    void readDevice(Device& d);
    void runDue(int64_t now);
    int64_t nextDue();
    void enqueue(int64_t due, const Event& e);
    void writeNow(Bus bus, const uint8_t* bytes, int len);
    void updateOutAvail();
    void wake();
    void loadJackPatterns();

    std::atomic<bool> running_{ false };
    std::string sdRoot_;
    std::vector<std::string> jackPatterns_;
    int64_t offsetUs_ = 0;
    int wakePipe_[2] = { -1, -1 };

    std::mutex devMutex_; // devices_ and every write to their fds
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<std::string> failedPaths_; // logged once, retried quietly

    std::mutex inMutex_;
    std::deque<MidiMessage> inQueue_[kBusCount];
    std::atomic<int> inCount_[kBusCount] = {};

    std::mutex schedMutex_;
    std::multimap<int64_t, Event> sched_; // equal times keep insertion order

    std::atomic<bool> outAvail_[kBusCount] = {};
    std::atomic<uint32_t> dropped_{ 0 };
};

#endif
