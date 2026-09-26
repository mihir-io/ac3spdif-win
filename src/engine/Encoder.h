// The encode stage, run in-process on the output device's render thread, and
// the drift controller that keeps two independent audio clocks in step.
//
// Placement is the point. The output thread asks for exactly the bytes its
// endpoint buffer needs; the encoder hands them out of the burst it is part
// way through sending and encodes the next frame only when that runs short.
// So the only encoded audio in flight is the rest of one burst — which is
// that burst being transmitted, not a buffer — and all the pipeline's slack
// is a margin of PCM ahead of the encoder, adjustable to the sample. The
// subprocess design this replaces kept a ring of whole encoded bursts
// between the encoder and the device, plus a spare burst for the sawtooth of
// an encoder handing them over one at a time.
//
// Clock drift is still the subtle problem. The capture endpoint and the
// S/PDIF endpoint run off different oscillators, and a bitstream cannot be
// resampled — every byte of a burst is load bearing — so the correction
// happens upstream, in the PCM domain, before anything is encoded: a frame
// is built from a sample more or fewer than nominal, dropping or repeating a
// single sample. The controller measures the PCM margin at the moment each
// frame is taken and keeps the smallest value seen over roughly a second —
// the trough is what decides whether a frame will starve. It is a PI loop:
// the proportional term removes that window's error over the next window,
// spread evenly rather than front-loaded, and the integral term learns the
// clock error itself and feeds it forward continuously. Proportional-only
// control settles a whole window's drift away from the target — 3 ms at
// 3000 ppm, measured, and on the slow side that is margin lost. Both
// directions are corrected, so a slow capture clock is padded instead of
// being left to run dry and drop out, and the learned rate survives pauses,
// since it is a property of the two crystals rather than of the audio.
//
// Everything reachable from pull() is realtime code: no locks, no logging on
// the success path, no allocation of our own. FFmpeg's encoder allocates its
// packet buffer per frame, which on a heap with a low-fragmentation front end
// costs microseconds; the worst encode time is measured and reported so that
// claim is checkable rather than assumed.

#pragma once

#include "../support/ByteRing.h"
#include "../support/Win32Util.h"
#include "BitstreamOutput.h"
#include "Config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct AVAudioFifo;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace ac3spdif {

struct EncoderStats {
    int64_t framesEncoded = 0;
    int64_t inputFramesConsumed = 0;
    int64_t samplesDropped = 0;
    int64_t samplesDuplicated = 0;
    // Frames encoded as silence because no PCM was there: idle (nothing
    // playing — loopback delivers no packets at all then) and dropouts alike.
    int64_t silenceFrames = 0;
    // Starvation episodes short enough to have been a gap in audio that was
    // otherwise playing — the figure that says the margin is too small.
    int64_t dropouts = 0;
    int64_t encodeErrors = 0;
    int64_t burstBytes = 0;
    // PCM margin at the trough of the last measurement window, in input
    // frames: what was left over once the frame had been taken.
    int marginFrames = 0;
    // Wall time spent encoding one frame on the render thread.
    double encodeMsAverage = 0;
    double encodeMsMax = 0;
};

class Encoder : public BurstSource {
public:
    // `inputRate`, `inputChannels` and `inputMask` are the capture geometry,
    // which may differ from the wire: a 96 kHz 7.1 mix feeding a 48 kHz 5.1
    // burst is legal, and libswresample converts in between. `pcmRing` is the
    // capture ring, or null for self-test, which synthesises tones instead.
    // `inputMask` 0 means the mix has no usable speaker mask.
    Encoder(const Config& config, double inputRate, int inputChannels,
            uint64_t inputMask, ByteRing* pcmRing);
    ~Encoder() override;

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Informational lines — FFmpeg's own errors among them. Never called from
    // pull() on the success path.
    std::function<void(const std::string&)> onLog;

    // Opens the codec, resampler and muxer. Throws Failure. Nothing here runs
    // on the render thread.
    void open();

    // Realtime: writes exactly `bytes` of IEC 61937 stream to `destination`.
    void pull(uint8_t* destination, size_t bytes) override;

    EncoderStats stats() const;
    // The encoder's own algorithmic delay, in wire-rate frames — a real
    // reading from libavcodec, not a constant.
    int encoderDelayFrames() const { return encoderDelayFrames_; }
    int frameSamples() const { return frameSamples_; }
    int targetMarginFrames() const { return targetMarginFrames_; }
    double inputRate() const { return inputRate_; }
    std::string lastError() const;

private:
    bool encodeNextBurst();
    bool produceFrame();
    // `measure` marks the first chunk of a frame, where the margin is sampled
    // and the correction applied — once per frame, however many chunks the
    // resampler's delay makes the frame take.
    bool takeInput(int nominal, bool measure);
    void synthesise(int frames);
    void feedResampler(const float* interleaved, int frames);
    void endWindow();
    void appendToCarry(const uint8_t* data, size_t bytes);
    void appendSilenceToCarry(size_t bytes);
    static int writePacketThunk(void* opaque, const uint8_t* data, int size);
    void writeDump(const uint8_t* data, size_t bytes);
    void dumpLoop();

    const Config config_;
    const double inputRate_;
    const int inputChannels_;
    const uint64_t inputMask_;
    ByteRing* const pcmRing_;

    AVCodecContext* codec_ = nullptr;
    AVFormatContext* muxer_ = nullptr;
    SwrContext* resampler_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    uint8_t** resampled_ = nullptr;
    int resampledCapacity_ = 0;

    int frameSamples_ = 0;
    int encoderDelayFrames_ = 0;
    int64_t nextPts_ = 0;

    // Input frames a nominal frame consumes: 1536 at 48 kHz for AC-3, 3072
    // from a 96 kHz mix. The margin and every correction are counted in
    // these units, since that is what the ring holds.
    int nominalInputFrames_ = 0;
    int targetMarginFrames_ = 0;
    int maxCorrectionFrames_ = 0;
    int deadbandFrames_ = 0;
    int backlogFrames_ = 0;
    std::vector<float> input_;

    // Drift controller state. Render thread only. The encoder starts out
    // starving: output begins at once, emitting silence until the margin has
    // filled, so start-up needs no special case and no timeout when nothing
    // is playing.
    bool starving_ = true;
    // Whether the current starvation episode interrupted playback, rather
    // than being the start-up fill. Only those can be dropouts.
    bool episodeFromPlayback_ = false;
    int episodeFrames_ = 0;
    int windowFrames_ = 0;
    int windowCount_ = 0;
    // The trough of the margin as it will stand once the proportional
    // correction still queued has been applied, and the raw trough, which is
    // what gets reported.
    int64_t windowMinimum_ = INT64_MAX;
    int64_t rawMinimum_ = INT64_MAX;
    // Proportional: frames still to drop (positive) or pad (negative).
    int64_t pendingCorrection_ = 0;
    // Integral: the clock error, in frames per window, fed forward a
    // fraction of a frame at a time.
    double driftPerWindow_ = 0;
    double feedforward_ = 0;
    int dropoutLimitFrames_ = 0;

    // Self-test tone state.
    std::vector<double> phase_;
    std::vector<double> phaseStep_;

    // The part of the current burst not yet handed to the device, plus room
    // for whatever the next encode appends.
    std::vector<uint8_t> carry_;
    size_t carryStart_ = 0;
    size_t carryLength_ = 0;

    // Dump tee: the render thread must not block on a disk, so bursts go
    // through a ring to a writer thread.
    std::unique_ptr<ByteRing> dumpRing_;
    Handle dumpFile_;
    std::thread dumpThread_;
    std::atomic<bool> dumpRunning_{false};

    std::atomic<int64_t> framesEncoded_{0};
    std::atomic<int64_t> inputFramesConsumed_{0};
    std::atomic<int64_t> samplesDropped_{0};
    std::atomic<int64_t> samplesDuplicated_{0};
    std::atomic<int64_t> silenceFrames_{0};
    std::atomic<int64_t> dropouts_{0};
    std::atomic<int64_t> encodeErrors_{0};
    std::atomic<int64_t> burstBytes_{0};
    std::atomic<int> marginFrames_{0};
    std::atomic<int64_t> encodeTicksTotal_{0};
    std::atomic<int64_t> encodeTicksMax_{0};
    int64_t ticksPerSecond_ = 1;
};

}  // namespace ac3spdif
