// System-audio capture via WASAPI loopback.
//
// This is the single biggest thing Windows does better for this pipeline:
// loopback capture is built into the OS, so there is no BlackHole-style
// virtual device to install, no kernel driver, and one less buffer hop for
// the audio to cross. Any render endpoint can be tapped for exactly what
// Windows is mixing to it, in the mix format the user's speaker configuration
// implies — set the speakers to 5.1 in Sound settings and loopback delivers
// 5.1 float, which is the same "Configure Speakers" step BlackHole needed on
// macOS, minus the third-party driver.
//
// The capture thread deinterleaves the channels we want into a scratch block
// and pushes them into a lock-free ring. Nothing on that thread allocates,
// locks or logs once the stream is running.

#pragma once

#include "../support/AudioDevices.h"
#include "../support/ByteRing.h"

#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <vector>

namespace ac3spdif {

class Capture {
public:
    // Per-channel peak meters cover every channel the *mix format* presents,
    // not just the ones being captured. That distinction is the whole point:
    // when a 5.1 capture is missing content, the first question is whether
    // the application put it on a channel beyond the ones we take, and
    // metering only the captured channels cannot answer that.
    static constexpr int maxMeteredChannels = 16;

    // Probes the endpoint's mix format immediately, so the encoder can be
    // configured with the real capture rate and channel count before any
    // stream exists. `channelCount` 0 means "match the mix".
    Capture(const DeviceInfo& device, int channelCount, ByteRing& ring);
    ~Capture();

    void start();  // throws Failure
    void stop();

    // What the mix format actually is — the encoder's input geometry.
    int sourceChannelCount() const { return mixChannels_; }
    int capturedChannelCount() const { return channelCount_; }
    double sampleRate() const { return mixSampleRate_; }
    DWORD channelMask() const { return mixChannelMask_; }

    // Frames per engine period — the capture-side latency term. Read back
    // from the device rather than assumed.
    int bufferFrames() const { return periodFrames_; }

    int64_t overflowFrames() const {
        return overflowFrames_.load(std::memory_order_relaxed);
    }

    // Current peak level per mix channel, 0...1, with a decay applied on the
    // capture thread so a short burst stays visible for a few blocks.
    //
    // Read without synchronisation on purpose: these are aligned 32-bit loads
    // of values the capture thread writes, and the worst outcome is a meter
    // one block stale. Taking a lock to avoid that would be the worse trade.
    std::vector<float> levels() const;
    // Peak per channel over the whole run, never decayed. The decaying meter
    // answers "what is playing now", which is useless once playback stops;
    // this answers "did anything ever arrive here", which is the question
    // actually being asked when a speaker is silent.
    std::vector<float> peakHold() const;

private:
    void captureLoop();
    void deliver(const BYTE* data, UINT32 frames, DWORD flags);

    DeviceInfo device_;
    int channelCount_;
    ByteRing& ring_;

    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> captureClient_;

    int mixChannels_ = 0;
    double mixSampleRate_ = 0;
    DWORD mixChannelMask_ = 0;
    int periodFrames_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> overflowFrames_{0};

    std::vector<float> scratch_;
    float peakLevels_[maxMeteredChannels] = {};
    float holdLevels_[maxMeteredChannels] = {};
};

}  // namespace ac3spdif
