#include "libultraship/libultraship.h"
#include "libultraship/bridge/consolevariablebridge.h" /* CVarGetInteger: live audio low-pass cutoff */
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ratio>
#include <vector>

// Establish a chrono duration for the N64 46.875MHz clock rate
typedef std::ratio<3000, 64> n64ClockRatio;
typedef std::ratio_divide<std::micro, n64ClockRatio> n64CycleRate;
typedef std::chrono::duration<long long, n64CycleRate> n64CycleRateDuration;

extern "C" {
uint8_t __osMaxControllers = MAXCONTROLLERS;
uint64_t __osCurrentTime = 0;

struct GdxDecompOSIoMesg {
    OSIoMesgHdr hdr;
    void* dramAddr;
    uint32_t devAddr;
    uint32_t size;
    OSPiHandle* piHandle;
};

/* Cartridge ROM image owned by the port layer, in raw .z64 cart byte order. Resolved when the
   executable links this static library. */
extern uint8_t* gdx_rom_buffer;
extern size_t gdx_rom_size;

/* port/gdx_segment_source.c, declared without its header because this file compiles in the
   libultraship target, which has no include path onto port/. Sources a cart read archive-first
   through the shared blob containment table, falling back to a byte-identical raw-ROM copy.
   Returns 0 only when no source can satisfy the read, leaving dst untouched. */
int GdxSegmentSourceRead(uint32_t romBase, uint32_t size, void* dst);

int32_t osContInit(OSMesgQueue* mq, uint8_t* controllerBits, OSContStatus* status) {
    *controllerBits = 0;
    status->status |= 1;

    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        SPDLOG_ERROR("Failed to initialize SDL game controllers ({})", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    Ship::Context::GetInstance()->GetControlDeck()->Init(controllerBits);

    return 0;
}

int32_t osContStartReadData(OSMesgQueue* mesg) {
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    memset(pad, 0, sizeof(OSContPad) * __osMaxControllers);

    Ship::Context::GetInstance()->GetControlDeck()->WriteToPad(pad);
}

void osSetTime(OSTime time) {
    __osCurrentTime =
        std::chrono::duration_cast<n64CycleRateDuration>(std::chrono::steady_clock::now().time_since_epoch()).count() +
        time;
}

// Returns the OS time matching the N64 46.875MHz cycle rate
uint64_t osGetTime() {
    return std::chrono::duration_cast<n64CycleRateDuration>(std::chrono::steady_clock::now().time_since_epoch())
               .count() -
           __osCurrentTime;
}

// Returns the CPU clock count matching the N64 46.875Mhz cycle rate
uint32_t osGetCount() {
    return std::chrono::duration_cast<n64CycleRateDuration>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

OSPiHandle* osCartRomInit() {
    static OSPiHandle sCartRomHandle = {};
    sCartRomHandle.type = DEVICE_TYPE_CART;
    sCartRomHandle.domain = PI_DOMAIN1;
    return &sCartRomHandle;
}

int osSetTimer(OSTimer* t, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    return 0;
}

int32_t osEPiStartDma(OSPiHandle* pihandle, OSIoMesg* mb, int32_t direction) {
    auto* decompMesg = reinterpret_cast<GdxDecompOSIoMesg*>(mb);
    if (decompMesg != nullptr) {
        decompMesg->piHandle = pihandle;

        if (direction == OS_READ && decompMesg->dramAddr != nullptr && decompMesg->size != 0) {
            /* devAddr is either a physical cart address (0x10000000-based) or a plain ROM
               offset; the mask covers both. Audio sample banks stream through here as the
               default AudioLoad DMA handler, so zero-filling them silences synthesis entirely.

               Reads go through the byte-source shim rather than straight out of gdx_rom_buffer
               so audio and non-audio resolve in one place. Its bounds check fails in exactly
               the cases the old code zero-filled, so keep the zero-fill: an out-of-range or
               absent read must still see deterministic contents. */
            const uint32_t romOffset = decompMesg->devAddr & 0x0FFFFFFFu;
            if (!GdxSegmentSourceRead(romOffset, decompMesg->size, decompMesg->dramAddr)) {
                memset(decompMesg->dramAddr, 0, decompMesg->size);
            }
        }

        if (decompMesg->hdr.retQueue != nullptr) {
            osSendMesg(decompMesg->hdr.retQueue, OS_MESG_PTR(mb), OS_MESG_NOBLOCK);
        }
    }

    return 0;
}

// port/gdx_audio_thread.cpp; declared without a header for the same reason as
// GdxSegmentSourceRead above.
extern "C" int gdx_audio_thread_active(void);

uint32_t osAiGetLength() {
    // Hardware returns the bytes still queued in the AI FIFO; the host backend's buffered frame
    // count stands in for it. Without this the game's adaptive fill (thread.c,
    // samplesRemainingInAi) always sees an empty AI and over-produces forever.
    auto audio = Ship::Context::GetInstance() != nullptr ? Ship::Context::GetInstance()->GetAudio() : nullptr;
    std::shared_ptr<Ship::AudioPlayer> player = audio != nullptr ? audio->GetAudioPlayer() : nullptr;
    if (player == nullptr || !player->IsInitialized()) {
        return 0;
    }
    // Host jitter cushion for the legacy per-VI-tick fiber producer only. That producer targets
    // the console AI FIFO depth, about one 60Hz frame, which host scheduling jitter empties
    // constantly -- every dip is an audible silence hole. Under-reporting the queued amount makes
    // the game's own fill math settle at target + cushion instead. 2048 frames is 64ms at 32kHz
    // and must stay well under SDLAudioPlayer::DoPlay's drop threshold.
    //
    // The dedicated audio thread produces off the actual buffered amount and cannot be stalled by
    // the game thread, so a cushion would only make it over-produce. Report honestly there, and
    // keep the exact old cushion on the kill-switch path so its behavior is unchanged.
    static int32_t sCushionFrames = -1;
    if (sCushionFrames < 0) {
        if (gdx_audio_thread_active()) {
            sCushionFrames = 0;
        } else {
            sCushionFrames = 2048;
            if (const char* env = std::getenv("GDX_AI_CUSHION")) {
                const long v = std::strtol(env, nullptr, 10);
                if (v >= 0 && v <= 4096) {
                    sCushionFrames = (int32_t)v;
                }
            }
        }
    }
    const int32_t buffered = (int32_t)player->Buffered() - sCushionFrames;
    return buffered > 0 ? (uint32_t)buffered * 4 : 0;
}

int32_t osAiSetNextBuffer(void* buff, size_t len) {
    // AI -> host wiring only: this plays back whatever bytes AudioThread_CreateTaskImpl left in
    // the buffer. The PCM itself comes from the port's aspMain ABI interpreter
    // (port/n64_audio_hle.c), because the decomp pipeline only ever builds RSP command lists.
    if (buff == nullptr || len == 0) {
        return 0;
    }
    auto audio = Ship::Context::GetInstance() != nullptr ? Ship::Context::GetInstance()->GetAudio() : nullptr;
    std::shared_ptr<Ship::AudioPlayer> player = audio != nullptr ? audio->GetAudioPlayer() : nullptr;

    bool allZero = true;

    const uint8_t* samples = static_cast<const uint8_t*>(buff);
    for (size_t k = 0; k < len; k++) {
        if (samples[k] != 0) {
            allZero = false;
            break;
        }
    }

    // Underrun resilience. A long synchronous game-thread operation starves the audio fiber
    // (port/n64_sched.c), AudioSynth_Update misses its tick, and the buffer arriving here is
    // all-zero -- audible as a click. Repeat the last buffer that had audio instead, halving its
    // gain per consecutive miss so a long stall decays to silence rather than looping a snippet.
    // Symptom relief only; the starvation itself is fixed by cooperative yields in Dma_LoadAssets
    // and mio0_decode.
    static std::vector<uint8_t> sLastGoodAiBuffer;
    static uint32_t sConsecutiveZeroAiBuffers = 0;
    std::vector<uint8_t> fadedSubstitute;
    const uint8_t* playBuf = static_cast<const uint8_t*>(buff);
    size_t playLen = len;

    // The dedicated audio thread does not share the starvation mode above, so an all-zero buffer
    // under it is legitimate silence and substituting a decaying copy of the last one turns that
    // into ghost notes. The last-good bookkeeping below stays keyed on !allZero rather than this
    // gate, so the fallback still has real audio to decay from if the kill switch flips at
    // runtime.
    const bool substituteFade = allZero && !gdx_audio_thread_active();
    if (substituteFade) {
        if (!sLastGoodAiBuffer.empty() && sLastGoodAiBuffer.size() == len && (len % sizeof(int16_t)) == 0) {
            ++sConsecutiveZeroAiBuffers;
            // Capped below 16 so the shift stays defined and a very long stall decays to
            // silence instead of looping at an audible volume.
            const uint32_t shift = sConsecutiveZeroAiBuffers > 15u ? 15u : sConsecutiveZeroAiBuffers;
            fadedSubstitute.resize(len);
            const int16_t* src = reinterpret_cast<const int16_t*>(sLastGoodAiBuffer.data());
            int16_t* dst = reinterpret_cast<int16_t*>(fadedSubstitute.data());
            const size_t numSamples = len / sizeof(int16_t);
            for (size_t s = 0; s < numSamples; s++) {
                dst[s] = static_cast<int16_t>(src[s] >> shift);
            }
            playBuf = fadedSubstitute.data();
            playLen = fadedSubstitute.size();
        }
    }
    if (!allZero) {
        sConsecutiveZeroAiBuffers = 0;
        if (sLastGoodAiBuffer.size() != len) {
            sLastGoodAiBuffer.resize(len);
        }
        std::memcpy(sLastGoodAiBuffer.data(), buff, len);
    }

    if (player == nullptr || !player->IsInitialized()) {
        return 0;
    }
    // Output reconstruction low-pass, modelling the N64 DAC output stage that a raw HLE
    // pipeline omits (measured +7.4dB of excess HF imaging near Nyquist without it). 4th-order
    // Butterworth as two cascaded biquads: steep, no ringing, midrange untouched. Applied to a
    // copy so the source buffer stays intact.
    {
        static int sLastHz = -1;               /* -1 = not yet configured */
        static int sEnabled = 0;
        static double sB[2][3], sA[2][2];      /* [section]{b0,b1,b2}, {a1,a2} (a0 normalized) */
        static double sZ[2][2][2];             /* [channel][section]{x/y history} -> use DF2T state */
        static std::vector<int16_t> sBuf;
        /* Read per buffer on the audio thread so a menu edit applies without a restart (benign
         * int race with the ImGui write). Coefficients are recomputed only on a change, and sZ is
         * deliberately kept across one so moving the slider does not click. */
        int hz = CVarGetInteger("gEnhancements.Audio.LowPassHz", 15000);
        if (hz < 0) { hz = 0; }
        if (hz >= 16000) { hz = 15999; }
        if (hz != sLastHz) {
            sLastHz = hz;
            sEnabled = (hz > 0) ? 1 : 0;
            if (sEnabled) {
                const double kPi = 3.14159265358979323846;
                const double w0 = 2.0 * kPi * (double)hz / 32000.0;
                const double cw = std::cos(w0), sw = std::sin(w0);
                const double Q[2] = { 0.54119610, 1.30656296 }; /* 4th-order Butterworth section Qs */
                for (int s = 0; s < 2; s++) {
                    double alpha = sw / (2.0 * Q[s]);
                    double a0 = 1.0 + alpha;
                    sB[s][0] = ((1.0 - cw) / 2.0) / a0;
                    sB[s][1] = (1.0 - cw) / a0;
                    sB[s][2] = ((1.0 - cw) / 2.0) / a0;
                    sA[s][0] = (-2.0 * cw) / a0;
                    sA[s][1] = (1.0 - alpha) / a0;
                }
            }
        }
        if (sEnabled && (playLen % 4) == 0) {
            const size_t frames = playLen / 4;
            sBuf.resize(frames * 2);
            const int16_t* src = reinterpret_cast<const int16_t*>(playBuf);
            for (int ch = 0; ch < 2; ch++) {
                for (size_t i = 0; i < frames; i++) {
                    double x = (double)src[2 * i + ch];
                    for (int s = 0; s < 2; s++) { /* transposed direct form II */
                        double y = sB[s][0] * x + sZ[ch][s][0];
                        sZ[ch][s][0] = sB[s][1] * x - sA[s][0] * y + sZ[ch][s][1];
                        sZ[ch][s][1] = sB[s][2] * x - sA[s][1] * y;
                        x = y;
                    }
                    long v = (long)(x >= 0 ? x + 0.5 : x - 0.5);
                    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                    sBuf[2 * i + ch] = (int16_t)v;
                }
            }
            playBuf = reinterpret_cast<const uint8_t*>(sBuf.data());
        }
    }
    // Master volume, after the low-pass so gain is the last thing done to the PCM. At vol == 100
    // the multiply is skipped entirely and playBuf is left untouched, so a default config is
    // sample-for-sample unmodified. Its own scratch vector, never in place, and guarded on
    // (playLen % 4) == 0 like the low-pass so a ragged length is never read as whole frames.
    {
        static std::vector<int16_t> sVolBuf;
        int vol = CVarGetInteger("gEnhancements.Audio.MasterVolume", 100);
        if (vol < 0) {
            vol = 0;
        }
        if (vol > 100) {
            vol = 100;
        }
        if (vol != 100 && (playLen % 4) == 0) {
            const size_t samples = playLen / sizeof(int16_t); /* interleaved stereo s16 */
            sVolBuf.resize(samples);
            const int16_t* src = reinterpret_cast<const int16_t*>(playBuf);
            const double gain = (double)vol / 100.0;
            for (size_t s = 0; s < samples; s++) {
                double scaled = (double)src[s] * gain;
                long v = (long)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5); /* round half away from 0 */
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sVolBuf[s] = (int16_t)v;
            }
            playBuf = reinterpret_cast<const uint8_t*>(sVolBuf.data());
        }
    }
    player->Play(playBuf, playLen);
    return 0;
}

int32_t __osMotorAccess(OSPfs* pfs, uint32_t vibrate) {
    auto io = Ship::Context::GetInstance()->GetControlDeck()->GetControllerByPort(pfs->channel)->GetRumble();
    if (vibrate) {
        io->StartRumble();
    } else {
        io->StopRumble();
    }

    return 0;
}

int32_t osMotorInit(OSMesgQueue* ctrlrqueue, OSPfs* pfs, int32_t channel) {
    pfs->channel = channel;
    return 0;
}
}
