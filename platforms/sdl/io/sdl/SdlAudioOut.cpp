#include "io/sdl/SdlAudioOut.h"

#include "midi/MidiHub.h"

#include <chrono>
#include <cstdio>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <cstdlib>
#include <cstring>
#include <string>

bool SdlAudioOut::start()
{
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "tracker-sdl");
    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
#ifdef POLYBRIK_T32
    // SDL's ALSA backend asks for 2 periods of `samples`. The synth archives have render spikes of
    // 5 to 8 ms (one WTFM block about 1.1 s after note-on, measured with synthhelper), so the device
    // gets 512-frame periods: 4 blocks (11.6 ms) are still queued when a callback starts.
    want.samples = 4 * AudioHost::kBlockFrames;
#else
    want.samples = AudioHost::kBlockFrames;
#endif
    want.callback = callback;
    want.userdata = this;
    // Only the buffer size may differ; SDL converts rate, format and channels itself otherwise.
    // SDL's ALSA backend opens $AUDIODEV. launch.sh points it at a USB audio interface when one is
    // plugged in; POLYBRIK_AUDIODEV_FALLBACK (space separated, for example "hw:0,0 default") is tried in
    // order when that device does not open, so a USB DAC that refuses the format falls back to the
    // Brick's own codec instead of leaving PolyBrik without sound.
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &obtained_, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (dev == 0)
    {
        const char* first = getenv("AUDIODEV");
        fprintf(stderr, "SDL_OpenAudioDevice(%s): %s\n", first ? first : "default", SDL_GetError());
        const char* fb = getenv("POLYBRIK_AUDIODEV_FALLBACK");
        std::string list = fb ? fb : "";
        size_t pos = 0;
        while (dev == 0 && pos < list.size())
        {
            size_t space = list.find(' ', pos); // ALSA names contain commas (hw:0,0)
            if (space == std::string::npos)
                space = list.size();
            const std::string name = list.substr(pos, space - pos);
            pos = space + 1;
            if (name.empty())
                continue;
            setenv("AUDIODEV", name.c_str(), 1);
            dev = SDL_OpenAudioDevice(nullptr, 0, &want, &obtained_, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
            if (dev == 0)
                fprintf(stderr, "SDL_OpenAudioDevice(%s): %s\n", name.c_str(), SDL_GetError());
        }
        if (dev == 0)
            return false;
    }
    {
        const char* used = getenv("AUDIODEV");
        fprintf(stderr, "audio out: %s, %d frames per period\n", used ? used : "default", obtained_.samples);
    }
    expectedPeriodUs = (int64_t)obtained_.samples * 1000000 / obtained_.freq;
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void SdlAudioOut::stop()
{
    if (dev == 0)
        return;
    SDL_CloseAudioDevice(dev); // waits for a running callback
    dev = 0;
}

void SdlAudioOut::fillFrames(int16_t* out, int frames)
{
    constexpr int kCarrySamples = 2 * AudioHost::kBlockFrames;
    int done = 0; // frames of this call already filled, to place each new block on the timeline
    while (frames > 0)
    {
        if (carryPos == kCarrySamples)
        {
            if (playBaseUs != 0)
                MidiHub::beginAudioBlock(playBaseUs + (int64_t)done * 1000000 / obtained_.freq);
            host.renderBlock(carry);
            MidiHub::endAudioBlock();
            carryPos = 0;
        }
        const int avail = (kCarrySamples - carryPos) / 2;
        const int n = frames < avail ? frames : avail;
        memcpy(out, carry + carryPos, (size_t)n * 2 * sizeof(int16_t));
        out += 2 * n;
        carryPos += 2 * n;
        frames -= n;
        done += n;
    }
}

void SdlAudioOut::callback(void* user, Uint8* stream, int len)
{
    SdlAudioOut* self = static_cast<SdlAudioOut*>(user);
#ifdef __linux__
    // SDL's Linux audio thread is SCHED_OTHER (realtime needs rtkit over D-Bus, absent in the static
    // build), so the MIDI device thread (SCHED_FIFO 40) and any busy thread could delay it. Raise it
    // once, above MIDI; as root on the device this works, elsewhere it fails and nothing changes.
    if (!self->rtTried)
    {
        self->rtTried = true;
        sched_param sp{};
        sp.sched_priority = 50;
        const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        fprintf(stderr, "audio out: callback thread SCHED_FIFO %d %s\n", sp.sched_priority, rc == 0 ? "ok" : "refused");
    }
#endif
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    if (self->lastCallbackUs != 0 && now - self->lastCallbackUs > self->expectedPeriodUs * 3 / 2)
        self->lateCallbacks_++;
    self->lastCallbackUs = now;
    self->callbacks_++;
    // SDL keeps about one more period in the device while this one is filled, so the first frame
    // written now is heard about one period from now. Sequencer MIDI output is held back to that time.
    // The callback wakes with some jitter, the device clock does not: the play time follows the frame
    // count from an anchor that tracks the measured time slowly, and jumps only after a dropout.
    {
        const int frames = len / (int)(2 * sizeof(int16_t));
        const int64_t measured = MidiHub::nowUs() + self->expectedPeriodUs;
        const int64_t predicted = self->anchorUs + self->anchorFrames * 1000000 / self->obtained_.freq;
        const int64_t error = measured - predicted;
        if (self->anchorUs == 0 || error > 20000 || error < -20000)
        {
            self->anchorUs = measured;
            self->anchorFrames = 0;
        }
        else
            self->anchorUs += error / 16;
        self->playBaseUs = self->anchorUs + self->anchorFrames * 1000000 / self->obtained_.freq;
        self->anchorFrames += frames;
    }
    self->fillFrames(reinterpret_cast<int16_t*>(stream), len / (int)(2 * sizeof(int16_t)));
    self->playBaseUs = 0;
}

void SdlAudioOut::printStats(FILE* to) const
{
    fprintf(to, "audio out: sdl device, %u callbacks, %u late callbacks (gap > 1.5x period; bursts count on CoreAudio), obtained %d Hz fmt 0x%x %d ch %u frames (%u bytes)\n",
            callbacks_, lateCallbacks_, obtained_.freq, obtained_.format, obtained_.channels,
            obtained_.samples, obtained_.size);
}
