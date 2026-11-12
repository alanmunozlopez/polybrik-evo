#include "midi/MidiHub.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/select.h>
#include <unistd.h>

namespace
{
constexpr int64_t kRescanUs = 1000000;   // hot-plug scan period
constexpr int64_t kMaxSleepUs = 20000;   // upper bound of one select() wait
constexpr int64_t kEarlyUs = 150;        // run events this close to their time now
constexpr size_t kInQueueMax = 4096;     // per bus; the oldest message is dropped past it

// Set on the audio thread between beginAudioBlock and endAudioBlock.
struct AudioBlockContext
{
    bool active = false;
    int64_t playUs = 0;        // when the block's first frame is heard
    int64_t renderStartUs = 0; // when its render started
};
thread_local AudioBlockContext tlsBlock;

std::string trim(std::string s)
{
    while (!s.empty() && isspace((unsigned char)s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i]))
        i++;
    return s.substr(i);
}

std::string firstLine(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return {};
    char buf[256];
    std::string s = fgets(buf, sizeof buf, f) ? buf : "";
    fclose(f);
    return trim(s);
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

// "midiC1D0" -> card 1, device 0.
bool parseRawMidiName(const char* n, int& card, int& dev)
{
    char tail;
    return sscanf(n, "midiC%dD%d%c", &card, &dev, &tail) == 2;
}

// Card id and the device's own name, for example "mio: mio MIDI 1" or "MiniLabmkII: Arturia MiniLab mkII".
std::string describeRawMidi(int card, int dev)
{
    char p[96];
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    std::string id = firstLine(p);
    snprintf(p, sizeof p, "/proc/asound/card%d/midi%d", card, dev);
    std::string name = firstLine(p);
    if (id.empty() && name.empty())
    {
        snprintf(p, sizeof p, "card %d device %d", card, dev);
        return p;
    }
    return id.empty() ? name : (name.empty() ? id : id + ": " + name);
}
} // namespace

MidiHub& MidiHub::instance()
{
    static MidiHub* hub = new MidiHub(); // never destroyed: the detached thread outlives main()
    return *hub;
}

int64_t MidiHub::nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void MidiHub::beginAudioBlock(int64_t playUs)
{
    tlsBlock.active = true;
    tlsBlock.playUs = playUs;
    tlsBlock.renderStartUs = nowUs();
}

void MidiHub::endAudioBlock()
{
    tlsBlock.active = false;
}

bool MidiHub::matchesAny(const std::string& name, const std::vector<std::string>& patterns)
{
    const std::string n = lower(name);
    for (const std::string& p : patterns)
        if (!p.empty() && n.find(lower(p)) != std::string::npos)
            return true;
    return false;
}

void MidiHub::loadJackPatterns()
{
    jackPatterns_.clear();
    if (sdRoot_.empty())
        return;
    FILE* f = fopen((sdRoot_ + "/midi-jack").c_str(), "r");
    if (!f)
        return;
    char buf[256];
    while (fgets(buf, sizeof buf, f))
    {
        std::string line = trim(buf);
        if (!line.empty() && line[0] != '#')
            jackPatterns_.push_back(line);
    }
    fclose(f);
}

void MidiHub::start(const char* sdRoot)
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    sdRoot_ = sdRoot ? sdRoot : "";
    loadJackPatterns();
    if (const char* off = getenv("POLYBRIK_MIDI_OFFSET_MS"))
        offsetUs_ = (int64_t)(atof(off) * 1000.0);
    if (pipe(wakePipe_) == 0)
    {
        fcntl(wakePipe_[0], F_SETFL, O_NONBLOCK);
        fcntl(wakePipe_[1], F_SETFL, O_NONBLOCK);
    }
    fprintf(stderr, "midi: raw MIDI host started (%zu midi-jack pattern(s), offset %lld us)\n",
            jackPatterns_.size(), (long long)offsetUs_);
    atexit([] { MidiHub::instance().allNotesOff(); });
    // pthread directly, not std::thread: the device build links a static glibc 2.28, where
    // std::thread can throw "Enable multithreading" unless libpthread is linked whole.
    pthread_t t;
    if (pthread_create(&t, nullptr, [](void* self) -> void* {
            static_cast<MidiHub*>(self)->threadMain();
            return nullptr;
        }, this) == 0)
        pthread_detach(t);
    else
    {
        fprintf(stderr, "midi: cannot start the device thread\n");
        running_ = false;
    }
}

// ---- input ------------------------------------------------------------------------------------

bool MidiHub::pop(Bus bus, MidiMessage& out)
{
    std::lock_guard<std::mutex> g(inMutex_);
    if (inQueue_[bus].empty())
        return false;
    out = inQueue_[bus].front();
    inQueue_[bus].pop_front();
    inCount_[bus].store((int)inQueue_[bus].size(), std::memory_order_release);
    return true;
}

void MidiHub::readDevice(Device& d)
{
    uint8_t buf[256];
    for (;;)
    {
        const ssize_t n = read(d.fd, buf, sizeof buf);
        if (n > 0)
        {
            std::lock_guard<std::mutex> g(inMutex_);
            std::deque<MidiMessage>& q = inQueue_[d.bus];
            for (ssize_t i = 0; i < n; i++)
            {
                MidiMessage m;
                if (!d.parser.feed(buf[i], m))
                    continue;
                if (m.type == 0xFE) // active sensing: nothing in the firmware uses it
                    continue;
                if (q.size() >= kInQueueMax)
                {
                    q.pop_front();
                    dropped_++;
                }
                q.push_back(m);
            }
            inCount_[d.bus].store((int)q.size(), std::memory_order_release);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        // 0 (EOF) or ENODEV / EIO: the device was unplugged.
        d.dead = true;
        return;
    }
}

// ---- output -----------------------------------------------------------------------------------

void MidiHub::writeNow(Bus bus, const uint8_t* bytes, int len)
{
    std::lock_guard<std::mutex> g(devMutex_);
    for (auto& d : devices_)
    {
        if (d->bus != bus || !d->canWrite || d->dead)
            continue;
        const ssize_t n = write(d->fd, bytes, (size_t)len);
        if (n == len)
            continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            d->dead = true; // the device thread closes it
        else
            dropped_++;     // output buffer full: the message is lost, as on a saturated DIN line
    }
}

void MidiHub::send(Bus bus, const uint8_t* bytes, int len)
{
    if (!running_ || !hasOutput(bus) || len <= 0 || len > 3)
        return;
    if (tlsBlock.active)
    {
        const int64_t due = tlsBlock.playUs + offsetUs_;
        if (due > nowUs() + kEarlyUs)
        {
            Event e;
            e.bus = bus;
            memcpy(e.bytes, bytes, (size_t)len);
            e.len = (uint8_t)len;
            enqueue(due, e);
            return;
        }
    }
    writeNow(bus, bytes, len);
}

void MidiHub::scheduleClock(uint32_t delayUs, void (*callback)())
{
    if (!callback)
        return;
    if (!running_)
    {
        callback();
        return;
    }
    const int64_t now = nowUs();
    // The firmware's delay is (sub-block offset - time since the block started rendering), so
    // adding the render time back gives the offset into the block, which then counts from its play time.
    const int64_t due = tlsBlock.active
                          ? tlsBlock.playUs + (now - tlsBlock.renderStartUs) + delayUs + offsetUs_
                          : now + delayUs;
    if (due <= now + kEarlyUs)
    {
        callback();
        return;
    }
    Event e;
    e.callback = callback;
    enqueue(due, e);
}

void MidiHub::cancelClock()
{
    std::lock_guard<std::mutex> g(schedMutex_);
    for (auto it = sched_.begin(); it != sched_.end();)
        it = it->second.callback ? sched_.erase(it) : std::next(it);
}

void MidiHub::allNotesOff()
{
    if (!running_)
        return;
    {
        std::lock_guard<std::mutex> g(schedMutex_);
        sched_.clear();
    }
    for (int bus = 0; bus < kBusCount; bus++)
    {
        if (!hasOutput((Bus)bus))
            continue;
        for (uint8_t ch = 1; ch <= 16; ch++)
        {
            uint8_t b[3];
            const int n = midiEncode(0xB0, ch, 123, 0, b);
            writeNow((Bus)bus, b, n);
        }
    }
}

void MidiHub::enqueue(int64_t due, const Event& e)
{
    {
        std::lock_guard<std::mutex> g(schedMutex_);
        sched_.emplace(due, e);
    }
    wake();
}

int64_t MidiHub::nextDue()
{
    std::lock_guard<std::mutex> g(schedMutex_);
    return sched_.empty() ? INT64_MAX : sched_.begin()->first;
}

void MidiHub::runDue(int64_t now)
{
    std::vector<Event> due;
    {
        std::lock_guard<std::mutex> g(schedMutex_);
        auto end = sched_.upper_bound(now + kEarlyUs);
        for (auto it = sched_.begin(); it != end; ++it)
            due.push_back(it->second);
        sched_.erase(sched_.begin(), end);
    }
    // Callbacks run without the lock: sendMidiClockImmediate calls back into send().
    for (const Event& e : due)
    {
        if (e.callback)
            e.callback();
        else
            writeNow(e.bus, e.bytes, e.len);
    }
}

void MidiHub::wake()
{
    if (wakePipe_[1] >= 0)
    {
        const uint8_t b = 1;
        if (write(wakePipe_[1], &b, 1) < 0) {} // full pipe: the thread is awake anyway
    }
}

// ---- devices ----------------------------------------------------------------------------------

void MidiHub::updateOutAvail()
{
    bool avail[kBusCount] = {};
    for (auto& d : devices_)
        if (d->canWrite && !d->dead)
            avail[d->bus] = true;
    for (int b = 0; b < kBusCount; b++)
        outAvail_[b].store(avail[b], std::memory_order_relaxed);
}

void MidiHub::openDevice(const std::string& path, const std::string& name)
{
    bool canRead = true, canWrite = true;
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0 && errno != EBUSY)
    {
        canWrite = false;
        fd = open(path.c_str(), O_RDONLY | O_NONBLOCK); // input-only port
    }
    if (fd < 0 && errno != EBUSY)
    {
        canRead = false;
        canWrite = true;
        fd = open(path.c_str(), O_WRONLY | O_NONBLOCK); // output-only port
    }
    if (fd < 0)
    {
        if (std::find(failedPaths_.begin(), failedPaths_.end(), path) == failedPaths_.end())
        {
            fprintf(stderr, "midi: cannot open %s (%s): %s\n", path.c_str(), name.c_str(), strerror(errno));
            failedPaths_.push_back(path);
        }
        return;
    }
    failedPaths_.erase(std::remove(failedPaths_.begin(), failedPaths_.end(), path), failedPaths_.end());
    std::unique_ptr<Device> d(new Device());
    d->path = path;
    d->name = name;
    d->fd = fd;
    d->canRead = canRead;
    d->canWrite = canWrite;
    d->bus = (matchesAny(name, jackPatterns_) || matchesAny(path, jackPatterns_)) ? kJack : kUsb;
    fprintf(stderr, "midi: + %s \"%s\" -> %s bus (%s%s)\n", path.c_str(), name.c_str(),
            d->bus == kJack ? "jack" : "usb", canRead ? "in" : "", canRead && canWrite ? "+out" : (canWrite ? "out" : ""));
    std::lock_guard<std::mutex> g(devMutex_);
    devices_.push_back(std::move(d));
    updateOutAvail();
}

void MidiHub::closeDead()
{
    std::lock_guard<std::mutex> g(devMutex_);
    bool changed = false;
    for (auto it = devices_.begin(); it != devices_.end();)
    {
        if (!(*it)->dead)
        {
            ++it;
            continue;
        }
        fprintf(stderr, "midi: - %s \"%s\"\n", (*it)->path.c_str(), (*it)->name.c_str());
        close((*it)->fd);
        it = devices_.erase(it);
        changed = true;
    }
    if (changed)
        updateOutAvail();
}

void MidiHub::rescan()
{
    std::vector<std::pair<std::string, std::string>> found; // path, name
    if (DIR* dir = opendir("/dev/snd"))
    {
        while (dirent* e = readdir(dir))
        {
            int card, dev;
            if (parseRawMidiName(e->d_name, card, dev))
                found.emplace_back(std::string("/dev/snd/") + e->d_name, describeRawMidi(card, dev));
        }
        closedir(dir);
    }
    if (const char* extra = getenv("POLYBRIK_MIDI_DEV"))
    {
        std::string list = extra;
        size_t pos = 0;
        while (pos <= list.size())
        {
            size_t colon = list.find(':', pos);
            if (colon == std::string::npos)
                colon = list.size();
            std::string p = list.substr(pos, colon - pos);
            if (!p.empty() && access(p.c_str(), F_OK) == 0)
                found.emplace_back(p, p);
            pos = colon + 1;
        }
    }
    std::sort(found.begin(), found.end());

    std::vector<std::string> open;
    {
        std::lock_guard<std::mutex> g(devMutex_);
        for (auto& d : devices_)
            open.push_back(d->path);
    }
    for (auto& f : found)
        if (std::find(open.begin(), open.end(), f.first) == open.end())
            openDevice(f.first, f.second);
}

void MidiHub::threadMain()
{
#ifdef __linux__
    // Clock out wants a timely wake-up while the audio thread renders on another core. The thread
    // only sleeps in select() and writes a few bytes, so a real-time priority cannot starve anything.
    // Root on the Brick; elsewhere this quietly fails and the thread stays SCHED_OTHER.
    sched_param sp{};
    sp.sched_priority = 40;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
        fprintf(stderr, "midi: device thread SCHED_FIFO %d\n", sp.sched_priority);
#endif
    int64_t nextScan = 0;
    for (;;)
    {
        int64_t now = nowUs();
        if (now >= nextScan)
        {
            rescan();
            nextScan = now + kRescanUs;
        }
        runDue(now);

        // Only this thread closes or adds fds, so the set stays valid while it waits.
        fd_set rd;
        FD_ZERO(&rd);
        int maxFd = -1;
        std::vector<Device*> readers;
        {
            std::lock_guard<std::mutex> g(devMutex_);
            for (auto& d : devices_)
                if (d->canRead && !d->dead && d->fd < FD_SETSIZE)
                {
                    FD_SET(d->fd, &rd);
                    maxFd = std::max(maxFd, d->fd);
                    readers.push_back(d.get());
                }
        }
        if (wakePipe_[0] >= 0)
        {
            FD_SET(wakePipe_[0], &rd);
            maxFd = std::max(maxFd, wakePipe_[0]);
        }

        now = nowUs();
        int64_t waitUs = std::min(nextScan, nextDue()) - now;
        waitUs = std::max<int64_t>(0, std::min<int64_t>(waitUs, kMaxSleepUs));
        timeval tv{ (time_t)(waitUs / 1000000), (suseconds_t)(waitUs % 1000000) };
        const int r = select(maxFd + 1, &rd, nullptr, nullptr, &tv);
        if (r > 0)
        {
            if (wakePipe_[0] >= 0 && FD_ISSET(wakePipe_[0], &rd))
            {
                uint8_t junk[64];
                while (read(wakePipe_[0], junk, sizeof junk) > 0) {}
            }
            for (Device* d : readers)
                if (FD_ISSET(d->fd, &rd))
                    readDevice(*d);
        }
        else if (r < 0 && errno == EBADF)
        {
            // A device fd went bad under select: find it by reading every port once.
            for (Device* d : readers)
                readDevice(*d);
        }
        closeDead();
    }
}
