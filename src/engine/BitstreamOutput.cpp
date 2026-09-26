#include "BitstreamOutput.h"

#include "Config.h"

#include <avrt.h>
#include <mmdeviceapi.h>

using Microsoft::WRL::ComPtr;

namespace ac3spdif {

static void storeFormat(std::vector<uint8_t>& bytes, const WAVEFORMATEX* format) {
    const size_t size = sizeof(WAVEFORMATEX) + format->cbSize;
    bytes.assign(reinterpret_cast<const uint8_t*>(format),
                 reinterpret_cast<const uint8_t*>(format) + size);
}

BitstreamOutput::BitstreamOutput(const DeviceInfo& device, BurstSource& source,
                                 CarrierMode preferredMode, double sampleRate,
                                 OutputCodec codec)
    : device_(device), source_(source), mode_(preferredMode),
      sampleRate_(sampleRate) {
    ComPtr<IAudioClient> client = activate();

    // Choose the exact format shape now, while nothing is claimed, so a
    // failure is a clean error rather than a device held half-configured.
    // Two shapes exist for the encoded carrier because drivers disagree about
    // which they honour; the probe answers, not a guess.
    if (preferredMode == CarrierMode::encoded) {
        const CandidateFormats candidates = encodedFormats(codec, sampleRate);
        if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                      &candidates.extensible.Format,
                                      nullptr) == S_OK) {
            storeFormat(formatBytes_, &candidates.extensible.Format);
            return;
        }
        if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                      &candidates.legacy, nullptr) == S_OK) {
            storeFormat(formatBytes_, &candidates.legacy);
            return;
        }
        // No encoded format: fall through to the PCM carrier rather than
        // refusing outright, mirroring how a device is ranked.
        mode_ = CarrierMode::pcm;
    }

    const WAVEFORMATEXTENSIBLE pcm = pcmCarrierFormat(sampleRate);
    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &pcm.Format,
                                  nullptr) == S_OK) {
        storeFormat(formatBytes_, &pcm.Format);
        return;
    }
    throw Failure(
        device_.name + " accepts neither an IEC 61937 format for " +
        codec::name(codec) + " nor 2ch/16-bit exclusive LPCM at " +
        std::to_string(static_cast<int>(sampleRate)) +
        " Hz. Run audioprobe to see what it does offer, or try --rate 44100.");
}

BitstreamOutput::~BitstreamOutput() { stop(); }

ComPtr<IAudioClient> BitstreamOutput::activate() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    throwIfFailed(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "cannot create the device enumerator");
    ComPtr<IMMDevice> device;
    throwIfFailed(enumerator->GetDevice(device_.id.c_str(), &device),
                  "output device \"" + device_.name + "\" disappeared");
    ComPtr<IAudioClient> client;
    throwIfFailed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   &client),
                  "cannot open an audio client on " + device_.name);
    return client;
}

// Claim the device exclusively at `period`, following the driver's alignment
// correction if it asks for one. Leaves client_, the event, the buffer size
// and the render client ready, or returns the failure.
HRESULT BitstreamOutput::claim(REFERENCE_TIME period) {
    renderClient_.Reset();
    client_ = activate();
    HRESULT hr = client_->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
        period, chosenFormat(), nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // The driver wants a buffer that lands on one of its own alignment
        // boundaries. It has just told us the size it wants; the documented
        // dance is to read it back, convert to a period, and start over with
        // a fresh client — a failed Initialize poisons the old one.
        UINT32 alignedFrames = 0;
        client_->GetBufferSize(&alignedFrames);
        period = static_cast<REFERENCE_TIME>(
            10000000.0 * alignedFrames / chosenFormat()->nSamplesPerSec + 0.5);
        client_ = activate();
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                 period, chosenFormat(), nullptr);
    }
    if (FAILED(hr)) return hr;
    renderEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(hr = client_->SetEventHandle(renderEvent_.get()))) return hr;
    if (FAILED(hr = client_->GetBufferSize(&bufferFrames_))) return hr;
    return client_->GetService(IID_PPV_ARGS(&renderClient_));
}

void BitstreamOutput::prepare() {
    client_ = activate();

    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    throwIfFailed(client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod),
                  "cannot read the device period of " + device_.name);
    if (defaultPeriod <= 0) defaultPeriod = minimumPeriod;

    // Event-driven exclusive mode at the device's *default* period, as the
    // driver reports it, never its advertised minimum. Drivers are free to
    // accept a shorter period and then pace to a clock of their own: the
    // Sound Blaster Omni does exactly that on its IEC 61937 path, firing
    // every 10 ms whatever it is given, and probing it there preceded a USB
    // stack bugcheck. The default is the period a driver actually honours.
    //
    // Nor is the buffer sized to a whole burst, the way a design that encodes
    // one burst per event has to. The encoder hands out bytes, not bursts, so
    // any period works — and a driver whose alignment rules force a period
    // that is not a multiple of the burst cannot push this loop into padding
    // between bursts, which would break the IEC 61937 burst spacing.
    const HRESULT hr = claim(defaultPeriod);

    if (hr == AUDCLNT_E_DEVICE_IN_USE) {
        throw Failure(device_.name +
                      " is already claimed in exclusive mode by another "
                      "process — often another instance of this tool, a "
                      "player doing its own passthrough, or the sound card's "
                      "own encoder software. Stop it and retry.");
    }
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
        throw Failure(
            "exclusive mode is disabled for " + device_.name +
            ". Open its properties in Sound settings and enable \"Allow "
            "applications to take exclusive control of this device\" — "
            "without it nothing can put a bitstream on the wire intact.");
    }
    throwIfFailed(hr, "cannot claim " + device_.name + " exclusively as " +
                          formatDescription(chosenFormat()));
}

void BitstreamOutput::begin() {
    // Fill the endpoint buffer before starting the stream, so the very first
    // thing the receiver sees is burst data rather than a gulp of nothing.
    BYTE* buffer = nullptr;
    if (SUCCEEDED(renderClient_->GetBuffer(bufferFrames_, &buffer)) && buffer) {
        const size_t bytes = static_cast<size_t>(bufferFrames_) * 4;
        source_.pull(buffer, bytes);
        renderedBytes_.fetch_add(static_cast<int64_t>(bytes),
                                 std::memory_order_relaxed);
        renderClient_->ReleaseBuffer(bufferFrames_, 0);
    }

    throwIfFailed(client_->Start(),
                  "cannot start the render stream on " + device_.name);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { renderLoop(); });
}

// Undo everything. Exclusive mode makes this almost nothing: stopping and
// releasing the client is the whole restoration, because nothing about the
// device was persistently changed. Tolerates being called when prepare() only
// partly succeeded.
void BitstreamOutput::stop() {
    running_.store(false, std::memory_order_release);
    if (renderEvent_) SetEvent(renderEvent_.get());
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    renderClient_.Reset();
    client_.Reset();
    renderEvent_.reset();
}

std::string BitstreamOutput::carrierDescription() const {
    return carrierName(mode_);
}

std::string BitstreamOutput::formatLabel() const {
    return formatBytes_.empty() ? std::string("(none)")
                                : formatDescription(chosenFormat());
}

std::string BitstreamOutput::failure() const {
    const HRESULT hr = lostWith_.load(std::memory_order_relaxed);
    if (hr == S_OK) return {};
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        return device_.name + " went away while streaming (unplugged, "
               "disabled, or reconfigured)";
    }
    return device_.name + " stopped accepting audio: " + hrDescription(hr);
}

// Render thread. Each event asks for exactly one period's bytes, and the
// encoder supplies them, encoding on this thread when the burst it is part
// way through sending runs short. In exclusive event-driven mode the endpoint
// is the stream, so there are no sibling buffers to silence the way a
// multi-stream CoreAudio device has.
void BitstreamOutput::renderLoop() {
    ComInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const size_t bytesPerPass = static_cast<size_t>(bufferFrames_) * 4;
    while (running_.load(std::memory_order_acquire)) {
        // The 2 s ceiling only matters if the device dies without an error;
        // the event fires every few milliseconds when things are healthy.
        if (WaitForSingleObject(renderEvent_.get(), 2000) != WAIT_OBJECT_0) {
            continue;
        }
        if (!running_.load(std::memory_order_acquire)) break;

        BYTE* buffer = nullptr;
        const HRESULT hr = renderClient_->GetBuffer(bufferFrames_, &buffer);
        if (FAILED(hr) || !buffer) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
                hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                // Spinning on a dead client would look like a hang; report
                // it and let the engine fail cleanly.
                lostWith_.store(hr, std::memory_order_relaxed);
                break;
            }
            continue;
        }
        source_.pull(buffer, bytesPerPass);
        renderedBytes_.fetch_add(static_cast<int64_t>(bytesPerPass),
                                 std::memory_order_relaxed);
        renderClient_->ReleaseBuffer(bufferFrames_, 0);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// MARK: - NullSink

void NullSink::begin() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
        ComInit com;
        // Paced by the wall clock, not by counting sleeps: Sleep(10) sleeps
        // *at least* ten milliseconds, and what it runs over would otherwise
        // accumulate as a pull rate below the wire rate — making the
        // diagnostic sink misreport exactly the margin behaviour it exists to
        // show.
        std::vector<uint8_t> scratch(480 * 4);
        const ULONGLONG startTick = GetTickCount64();
        int64_t pulled = 0;
        while (running_.load(std::memory_order_acquire)) {
            Sleep(10);
            const int64_t due = static_cast<int64_t>(
                (GetTickCount64() - startTick) * bytesPerSecond_ / 1000.0);
            while (due - pulled >= static_cast<int64_t>(scratch.size())) {
                source_.pull(scratch.data(), scratch.size());
                pulled += static_cast<int64_t>(scratch.size());
                renderedBytes_.fetch_add(static_cast<int64_t>(scratch.size()),
                                         std::memory_order_relaxed);
            }
        }
    });
}

void NullSink::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

}  // namespace ac3spdif
