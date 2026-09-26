// The S/PDIF sink.
//
// Getting a bitstream out of Windows intact means taking the endpoint away
// from the audio engine entirely, and WASAPI exclusive mode is exactly that in
// one step: no mixer, no volume scaling, no sample-rate conversion, no other
// client able to open the device — what took hog mode plus a physical format
// switch plus a mixing property on macOS is a single Initialize call here. And
// where CoreAudio has an encoded stream format for AC-3 alone, Windows defines
// IEC 61937 exclusive formats per codec, so DTS gets a first-class carrier
// with the non-audio bit set instead of having to hide in PCM.
//
// Teardown is equally simpler: releasing the client releases the device. There
// is no format to restore and no hog mode to give back, so a crash cannot
// leave the device stuck in a passthrough format the way it can on macOS.
//
// The render loop is event-driven at the device's period — 10 ms on the
// hardware measured — and each event pulls exactly one period's bytes from
// the encoder, which encodes on this thread when it needs to. Nothing sits
// between the encoder and the endpoint buffer.

#pragma once

#include "../support/AudioDevices.h"

#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace ac3spdif {

// Where a sink's bytes come from: an encoder, pulled on the sink's own thread.
class BurstSource {
public:
    virtual ~BurstSource() = default;
    // Realtime: write exactly `bytes` of IEC 61937 stream to `destination`.
    virtual void pull(uint8_t* destination, size_t bytes) = 0;
};

// What the engine needs from a destination. Two implementations: the real
// device, and a discard sink used for diagnosis.
class BitstreamSink {
public:
    virtual ~BitstreamSink() = default;
    // Claim the device and negotiate the format. Split from begin() so the
    // engine can report the negotiated format and finish its own set-up with
    // the device already held.
    virtual void prepare() = 0;
    virtual void begin() = 0;
    virtual void stop() = 0;
    virtual int64_t renderedBytes() const = 0;
    // Wire rate of the carrier, used to turn a byte count back into seconds.
    virtual double bytesPerSecond() const = 0;
    virtual std::string deviceName() const = 0;
    virtual std::string carrierDescription() const = 0;
    virtual std::string formatLabel() const = 0;
    virtual bool isPCMCarrier() const = 0;
    // The endpoint buffer in frames — a real latency term, read back from the
    // device rather than assumed.
    virtual int bufferFrames() const = 0;
    // Set when the device went away underneath the stream (unplugged,
    // disabled, claimed by a format change). Empty while healthy.
    virtual std::string failure() const = 0;
};

class BitstreamOutput : public BitstreamSink {
public:
    BitstreamOutput(const DeviceInfo& device, BurstSource& source,
                    CarrierMode preferredMode, double sampleRate,
                    OutputCodec codec);  // throws Failure
    ~BitstreamOutput() override;

    void prepare() override;
    void begin() override;
    void stop() override;

    int64_t renderedBytes() const override {
        return renderedBytes_.load(std::memory_order_relaxed);
    }
    double bytesPerSecond() const override { return sampleRate_ * 4; }
    std::string deviceName() const override { return device_.name; }
    std::string carrierDescription() const override;
    std::string formatLabel() const override;
    bool isPCMCarrier() const override { return mode_ == CarrierMode::pcm; }
    int bufferFrames() const override { return static_cast<int>(bufferFrames_); }
    std::string failure() const override;

    CarrierMode mode() const { return mode_; }

private:
    void renderLoop();
    HRESULT claim(REFERENCE_TIME period);
    Microsoft::WRL::ComPtr<IAudioClient> activate();
    const WAVEFORMATEX* chosenFormat() const {
        return reinterpret_cast<const WAVEFORMATEX*>(formatBytes_.data());
    }

    DeviceInfo device_;
    BurstSource& source_;
    CarrierMode mode_;
    double sampleRate_;

    // The negotiated format, stored as bytes because WAVEFORMATEX is
    // variable-length: the extensible IEC 61937 shape and the legacy tagged
    // shape are different sizes and drivers accept one or the other.
    std::vector<uint8_t> formatBytes_;

    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient_;
    Handle renderEvent_;
    UINT32 bufferFrames_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> renderedBytes_{0};
    std::atomic<HRESULT> lostWith_{S_OK};
};

// Encodes normally and throws the bitstream away, pacing the pull at the real
// wire rate so margin and drift figures stay meaningful.
//
// This exists so the capture half can be examined — channel levels, channel
// mapping, what a source is actually sending — without taking exclusive
// control of an audio device. Debugging a silent speaker should not require
// stopping whatever is currently using the hardware, and pointing a bitstream
// at an analog output to "just have a look" is how people blow up speakers.
class NullSink : public BitstreamSink {
public:
    NullSink(BurstSource& source, double sampleRate)
        : source_(source), bytesPerSecond_(sampleRate * 4) {}
    ~NullSink() override { stop(); }

    void prepare() override {}
    void begin() override;
    void stop() override;

    int64_t renderedBytes() const override {
        return renderedBytes_.load(std::memory_order_relaxed);
    }
    double bytesPerSecond() const override { return bytesPerSecond_; }
    std::string deviceName() const override { return "none (diagnostic)"; }
    std::string carrierDescription() const override { return "discard"; }
    std::string formatLabel() const override { return "encoded, then discarded"; }
    bool isPCMCarrier() const override { return false; }
    // The pull quantum stands in for an endpoint buffer.
    int bufferFrames() const override { return 480; }
    std::string failure() const override { return {}; }

private:
    BurstSource& source_;
    double bytesPerSecond_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> renderedBytes_{0};
};

}  // namespace ac3spdif
