#include "Capture.h"

#include <avrt.h>
#include <mmdeviceapi.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

namespace ac3spdif {

// One reference-time unit is 100 ns.
static constexpr REFERENCE_TIME kReftimesPerMillisecond = 10000;

Capture::Capture(const DeviceInfo& device, int channelCount, ByteRing& ring)
    : device_(device), channelCount_(channelCount), ring_(ring) {
    // The mix format is known before any stream exists, so the encoder can be
    // configured with the real capture geometry rather than a hope.
    mixChannels_ = device.mixChannels;
    mixSampleRate_ = device.mixSampleRate;
    mixChannelMask_ = device.mixChannelMask;
    if (channelCount_ <= 0) {
        // Match the mix, capped at 8: AC-3 encodes at most 5.1 and an
        // 8-channel capture already folds the side pair in; channels past
        // eight have nothing to fold into.
        channelCount_ = mixChannels_ < 8 ? mixChannels_ : 8;
    }
}

Capture::~Capture() { stop(); }

void Capture::start() {
    if (channelCount_ > mixChannels_) {
        throw Failure(
            device_.name + " mixes only " + std::to_string(mixChannels_) +
            " channel(s) but " + std::to_string(channelCount_) +
            " were requested. Open Sound settings, choose the device, and set "
            "the speaker configuration to 5.1 or 7.1 — the mix format is what "
            "loopback capture delivers, and it follows that setting.");
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    throwIfFailed(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "cannot create the device enumerator");
    ComPtr<IMMDevice> device;
    throwIfFailed(enumerator->GetDevice(device_.id.c_str(), &device),
                  "capture device \"" + device_.name + "\" disappeared");
    throwIfFailed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   &client_),
                  "cannot open an audio client on " + device_.name);

    WAVEFORMATEX* mix = nullptr;
    throwIfFailed(client_->GetMixFormat(&mix),
                  "cannot read the mix format of " + device_.name);
    // Loopback hands out the shared engine's own format, which is 32-bit
    // float interleaved on every Windows this targets. Guarded anyway: a
    // format this code does not expect must stop the pipeline, not corrupt it.
    const bool isFloat32 =
        (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 32) ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         mix->wBitsPerSample == 32 &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat.Data1 == 3);
    if (!isFloat32) {
        const std::string description = formatDescription(mix);
        CoTaskMemFree(mix);
        throw Failure("the mix format of " + device_.name + " is " +
                      description +
                      "; loopback capture is expected to deliver 32-bit float");
    }

    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    periodFrames_ = static_cast<int>(defaultPeriod * mixSampleRate_ /
                                     10000000.0 + 0.5);

    // The buffer is generous — 200 ms — because its size is slack, not
    // latency: the capture thread drains every packet the moment it wakes, so
    // steady-state latency is one engine period plus the polling cadence.
    // What a big buffer buys is survival of a scheduling hiccup without
    // AUDCLNT_E_BUFFER_ERROR taking the stream down.
    const HRESULT hr = client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
        200 * kReftimesPerMillisecond, 0, mix, nullptr);
    CoTaskMemFree(mix);
    throwIfFailed(hr, "cannot start loopback capture on " + device_.name);

    UINT32 bufferFrames = 0;
    throwIfFailed(client_->GetBufferSize(&bufferFrames),
                  "cannot size the capture buffer of " + device_.name);
    scratch_.resize(static_cast<size_t>(bufferFrames) * channelCount_);

    throwIfFailed(client_->GetService(IID_PPV_ARGS(&captureClient_)),
                  "cannot obtain the capture client of " + device_.name);
    throwIfFailed(client_->Start(),
                  "cannot start the capture stream on " + device_.name);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { captureLoop(); });
}

void Capture::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    captureClient_.Reset();
    client_.Reset();
}

std::vector<float> Capture::levels() const {
    const int count = mixChannels_ < maxMeteredChannels ? mixChannels_
                                                        : maxMeteredChannels;
    return std::vector<float>(peakLevels_, peakLevels_ + count);
}

std::vector<float> Capture::peakHold() const {
    const int count = mixChannels_ < maxMeteredChannels ? mixChannels_
                                                        : maxMeteredChannels;
    return std::vector<float>(holdLevels_, holdLevels_ + count);
}

void Capture::captureLoop() {
    ComInit com;

    // MMCSS moves this thread into the same scheduling class the audio engine
    // itself runs in. Without it a busy desktop can preempt the drain long
    // enough to overflow the capture buffer.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // Loopback capture does not signal events. The documented WASAPI
    // limitation is that AUDCLNT_STREAMFLAGS_EVENTCALLBACK never fires for a
    // loopback stream (the workaround Microsoft describes is a dummy render
    // stream used purely as a metronome). Polling on a high-resolution
    // waitable timer is simpler and costs nothing measurable: at a 2 ms
    // cadence the thread wakes five times per engine period and drains
    // whatever has arrived, so the added latency is the cadence itself.
    Handle timer(CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS));
    LARGE_INTEGER due;
    due.QuadPart = -2 * kReftimesPerMillisecond;
    if (timer) SetWaitableTimer(timer.get(), &due, 2, nullptr, nullptr, FALSE);

    while (running_.load(std::memory_order_acquire)) {
        if (timer) WaitForSingleObject(timer.get(), 10);
        else Sleep(2);

        UINT32 packetFrames = 0;
        while (SUCCEEDED(captureClient_->GetNextPacketSize(&packetFrames)) &&
               packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags,
                                                 nullptr, nullptr))) {
                break;
            }
            deliver(data, frames, flags);
            captureClient_->ReleaseBuffer(frames);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// Capture-thread path. Meter every mix channel, deinterleave the leading
// `channelCount_` channels into the scratch block, hand it to the ring. A
// short write means the encoder has fallen behind; the lost frames are
// counted rather than blocked on.
void Capture::deliver(const BYTE* data, UINT32 frames, DWORD flags) {
    if (frames == 0) return;
    const size_t needed = static_cast<size_t>(frames) * channelCount_;
    if (needed > scratch_.size()) return;

    // Decay the live meters so a tone that stops fades instead of latching.
    // The undecayed hold is what makes it findable afterwards.
    for (int channel = 0; channel < maxMeteredChannels; ++channel) {
        peakLevels_[channel] *= 0.95f;
    }

    // Windows marks a silent packet rather than always zeroing it; the data
    // pointer is valid but its contents are unspecified, so silence has to be
    // written, not copied.
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        std::fill(scratch_.begin(), scratch_.begin() + needed, 0.0f);
    } else {
        const float* source = reinterpret_cast<const float*>(data);
        const int meterable = mixChannels_ < maxMeteredChannels
                                  ? mixChannels_ : maxMeteredChannels;
        for (int channel = 0; channel < meterable; ++channel) {
            float peak = 0;
            for (UINT32 frame = 0; frame < frames; ++frame) {
                const float sample = source[frame * mixChannels_ + channel];
                const float magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak) peak = magnitude;
            }
            if (peak > 1) peak = 1;
            if (peak > peakLevels_[channel]) peakLevels_[channel] = peak;
            if (peak > holdLevels_[channel]) holdLevels_[channel] = peak;
        }
        for (UINT32 frame = 0; frame < frames; ++frame) {
            const float* in = source + static_cast<size_t>(frame) * mixChannels_;
            float* out = scratch_.data() + static_cast<size_t>(frame) * channelCount_;
            for (int channel = 0; channel < channelCount_; ++channel) {
                out[channel] = in[channel];
            }
        }
    }

    // Whole frames only. A ring that fills must drop frames, never split one:
    // a partial frame shifts the channel interleave for the rest of the
    // session, and front-left comes out of the centre speaker for good.
    const size_t frameBytes = static_cast<size_t>(channelCount_) * sizeof(float);
    size_t writable = ring_.freeSpace() / frameBytes;
    if (writable > frames) writable = frames;
    ring_.write(scratch_.data(), writable * frameBytes);
    if (writable < frames) {
        overflowFrames_.fetch_add(static_cast<int64_t>(frames - writable),
                                  std::memory_order_relaxed);
    }
}

}  // namespace ac3spdif
