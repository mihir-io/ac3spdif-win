#include "Engine.h"

#include "Encoder.h"
#include "LatencyAdvice.h"

#include <timeapi.h>

namespace ac3spdif {

Engine::Engine(const Config& config_) : config(config_) {
    finished_.reset(CreateEventW(nullptr, TRUE, TRUE, nullptr));
}

Engine::~Engine() {
    stop();
    waitUntilStopped(8);
    if (worker_.joinable()) worker_.join();
}

void Engine::start() {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (isActive(status_.state)) return;
        status_ = EngineStatus();
        status_.state = EngineState::starting;
        stopRequested_ = false;
        warnedAboutDiscardedChannels_ = false;
    }
    if (onStateChange) onStateChange(EngineState::starting);

    ResetEvent(finished_.get());
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] { run(); });
}

void Engine::stop() {
    std::lock_guard<std::mutex> guard(lock_);
    if (!isActive(status_.state)) return;
    stopRequested_ = true;
}

bool Engine::waitUntilStopped(double timeoutSeconds) {
    return WaitForSingleObject(finished_.get(),
                               static_cast<DWORD>(timeoutSeconds * 1000)) ==
           WAIT_OBJECT_0;
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> guard(lock_);
    return status_;
}

EngineState Engine::state() const {
    std::lock_guard<std::mutex> guard(lock_);
    return status_.state;
}

std::vector<float> Engine::liveInputLevels() const {
    std::lock_guard<std::mutex> guard(lock_);
    return activeCapture_ ? activeCapture_->levels() : std::vector<float>();
}

void Engine::setState(EngineState state, const std::string& reason) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        status_.state = state;
        if (state == EngineState::failed) status_.failureReason = reason;
    }
    if (onStateChange) onStateChange(state);
}

void Engine::log(const std::string& message) {
    if (onLog) onLog(message);
}

bool Engine::shouldStop() const {
    std::lock_guard<std::mutex> guard(lock_);
    return stopRequested_;
}

void Engine::run() {
    ComInit com;
    // 1 ms timer resolution for the whole run. The capture drain and the
    // diagnostic sink pace themselves with short waits; at the default
    // 15.6 ms quantum a "2 ms" wait is really a scheduling roulette, and the
    // drift controller would see it as phantom jitter.
    timeBeginPeriod(1);
    try {
        startPipeline();
    } catch (const Failure& failure) {
        setState(EngineState::failed, failure.what());
    } catch (const std::exception& error) {
        setState(EngineState::failed, error.what());
    }
    timeEndPeriod(1);
    SetEvent(finished_.get());
}

// The speaker mask of the leading `channels` channels of a mix — what the
// capture thread actually hands over when it takes fewer channels than the
// mix presents. WASAPI channel order follows the mask's bit order, so the
// first N set bits are the first N channels. Zero when the mask cannot
// describe them.
static uint64_t leadingMask(uint64_t mask, int channels) {
    uint64_t out = 0;
    int taken = 0;
    for (int bit = 0; bit < 64 && taken < channels; ++bit) {
        if (mask & (1ull << bit)) {
            out |= 1ull << bit;
            ++taken;
        }
    }
    return taken == channels ? out : 0;
}

// Runs every teardown step in reverse order on scope exit, bracketed by the
// state transitions. A failure recorded before the guard fires survives it:
// the transition to `stopping` must not erase the reason the run ended, or
// the UI shows a clean stop where there was an error.
class TeardownGuard {
public:
    TeardownGuard(std::function<EngineState()> currentState,
                  std::function<void(EngineState, const std::string&)> setState,
                  std::function<std::string()> failureReason)
        : currentState_(std::move(currentState)),
          setState_(std::move(setState)),
          failureReason_(std::move(failureReason)) {}
    ~TeardownGuard() {
        const bool failed = currentState_() == EngineState::failed;
        const std::string reason = failed ? failureReason_() : "";
        setState_(EngineState::stopping, "");
        for (auto step = steps_.rbegin(); step != steps_.rend(); ++step) {
            (*step)();
        }
        if (failed) setState_(EngineState::failed, reason);
        else setState_(EngineState::idle, "");
    }
    void add(std::function<void()> step) { steps_.push_back(std::move(step)); }

private:
    std::vector<std::function<void()>> steps_;
    std::function<EngineState()> currentState_;
    std::function<void(EngineState, const std::string&)> setState_;
    std::function<std::string()> failureReason_;
};

void Engine::startPipeline() {
    // Resolve devices before anything is claimed, so every failure up to the
    // real start is side-effect free.
    DeviceInfo inputDevice;
    const bool hasCapture = !config.selfTest;
    if (hasCapture) {
        if (!AudioDevices::find(config.inputQuery, config.sampleRate,
                                inputDevice)) {
            throw Failure(config.inputQuery.empty()
                              ? "no default output device to capture from"
                              : "no audio device matches \"" +
                                    config.inputQuery + "\"");
        }
    }

    DeviceInfo outputDevice;
    const bool discardOutput = lowered(config.outputQuery) == "none";
    if (!discardOutput) {
        if (!config.outputQuery.empty()) {
            if (!AudioDevices::find(config.outputQuery, config.sampleRate,
                                    outputDevice)) {
                throw Failure("no audio device matches \"" +
                              config.outputQuery + "\"");
            }
        } else {
            // Only auto-select a device whose driver takes an encoded
            // format. A PCM carrier is indistinguishable from a headphone
            // jack, and guessing wrong means full-scale digital noise out of
            // somebody's speakers; if that is really wanted, it must be named.
            const auto ranked = AudioDevices::rankedBitstreamSinks(
                config.sampleRate, config.codec, false);
            if (ranked.empty()) {
                throw Failure(
                    "no device accepts an IEC 61937 " +
                    std::string(codec::name(config.codec)) + " format at " +
                    std::to_string(static_cast<int>(config.sampleRate)) +
                    " Hz in exclusive mode. Choose a device explicitly and, "
                    "if it has no encoded format, switch the carrier to PCM.");
            }
            outputDevice = ranked.front().device;
            if (ranked.size() > 1) {
                std::string others;
                for (size_t i = 1; i < ranked.size(); ++i) {
                    if (!others.empty()) others += ", ";
                    others += ranked[i].device.name;
                }
                log("also bitstream-capable: " + others);
            }
        }

        // Loopback taps the shared engine, and exclusive mode shuts the
        // shared engine out — the two cannot coexist on one endpoint. The
        // usual setup is: Windows' default output is the speakers or TV, and
        // the bitstream goes to the S/PDIF endpoint this claims.
        if (hasCapture && inputDevice.id == outputDevice.id) {
            throw Failure(
                "the capture source and the bitstream output are the same "
                "endpoint (" + outputDevice.name + "). Loopback capture taps "
                "what Windows mixes to a device, and exclusive mode shuts "
                "that mixer out, so one endpoint cannot do both. Set the "
                "Windows default output to a different device and capture "
                "from that.");
        }
    }

    const double inputRate =
        hasCapture ? inputDevice.mixSampleRate : config.sampleRate;

    // One second of capture, sized in whole frames of what the capture
    // thread writes. The margin is a few tens of milliseconds of it; the rest
    // is room to survive a render stall without dropping audio.
    std::unique_ptr<ByteRing> pcmRing;
    std::unique_ptr<Capture> capture;
    int inputChannels = config.selfTestChannels();
    uint64_t inputMask = 0;
    if (hasCapture) {
        const int wanted = config.channelCount > 0
                               ? config.channelCount
                               : (inputDevice.mixChannels < 8
                                      ? inputDevice.mixChannels : 8);
        pcmRing = std::make_unique<ByteRing>(
            static_cast<size_t>(wanted) * sizeof(float) *
            static_cast<size_t>(inputRate > 0 ? inputRate : 48000));
        capture = std::make_unique<Capture>(inputDevice, wanted, *pcmRing);
        inputChannels = capture->capturedChannelCount();
        inputMask = leadingMask(inputDevice.mixChannelMask, inputChannels);
    }

    Encoder encoder(config, inputRate, inputChannels, inputMask, pcmRing.get());
    encoder.onLog = [this](const std::string& line) { log(line); };
    encoder.open();

    std::unique_ptr<BitstreamSink> output;
    if (discardOutput) {
        output = std::make_unique<NullSink>(encoder, config.sampleRate);
    } else {
        output = std::make_unique<BitstreamOutput>(
            outputDevice, encoder, config.carrier, config.sampleRate,
            config.codec);
    }

    {
        std::lock_guard<std::mutex> guard(lock_);
        activeCapture_ = capture.get();
        status_.inputName = capture
                                ? inputDevice.name + " (loopback)"
                                : "Self-test generator";
        status_.outputName = output->deviceName();
        status_.carrier = output->carrierDescription();
        status_.format = output->formatLabel();
        status_.targetMarginMs = hasCapture ? config.bufferMs : 0;
        status_.capturedChannels = inputChannels;
        status_.sourceChannelMask = capture ? capture->channelMask() : 0;
    }

    if (hasCapture) {
        const int floor = LatencyAdvice::minimumBufferMs(
            &inputDevice, discardOutput ? nullptr : &outputDevice,
            config.codec, config.sampleRate);
        if (config.bufferMs < floor) {
            log("buffer is " + std::to_string(config.bufferMs) +
                " ms; the floor for this capture device is " +
                std::to_string(floor) + " ms — expect dropouts.");
        }
    }
    if (config.bitrateWasSubstituted()) {
        std::string accepted;
        for (const auto& rate : codec::supportedBitrates(config.codec)) {
            if (!accepted.empty()) accepted += ", ";
            accepted += rate;
        }
        log(std::string(codec::name(config.codec)) + " does not accept " +
            config.bitrate + "; using " + config.effectiveBitrate() +
            " instead. Accepted: " + accepted + ".");
    }
    if (inputChannels > Config::maxEncodedChannels) {
        log("capturing " + std::to_string(inputChannels) +
            " channels, but AC-3 has no 7.1 mode: the encoder folds the side "
            "pair into the back pair and sends 5.1.");
    }
    if (hasCapture && inputRate != config.sampleRate) {
        log("the mix runs at " + std::to_string(static_cast<int>(inputRate)) +
            " Hz and the wire at " +
            std::to_string(static_cast<int>(config.sampleRate)) +
            " Hz; the encoder resamples in between.");
    }
    if (output->isPCMCarrier()) {
        log("PCM carrier: the IEC 60958 non-audio bit will NOT be set, so the "
            "receiver must detect the burst preamble itself. If this stream "
            "reaches an ANALOG output it will be full-scale noise. (Exclusive "
            "mode still bypasses the mixer, so the bytes themselves are "
            "bit-exact.)");
    }

    TeardownGuard teardown(
        [this] { return state(); },
        [this](EngineState s, const std::string& r) { setState(s, r); },
        [this] {
            std::lock_guard<std::mutex> guard(lock_);
            return status_.failureReason;
        });

    if (capture) {
        capture->start();
        teardown.add([this, &capture] {
            capture->stop();
            std::lock_guard<std::mutex> guard(lock_);
            activeCapture_ = nullptr;
        });
    }

    output->prepare();
    teardown.add([&output] { output->stop(); });

    // Nothing to prebuffer: the encoder starts out starving, so the wire
    // carries encoded silence until the margin has filled, and the first
    // audible frame arrives with its cushion intact. No timeout either —
    // when nothing is playing, loopback delivers nothing, and silence on the
    // wire is the right answer to that.
    output->begin();

    const auto toWireFrames = [&](double frames, double rate) {
        return rate > 0 ? static_cast<int>(frames * config.sampleRate / rate + 0.5)
                        : 0;
    };
    {
        std::lock_guard<std::mutex> guard(lock_);
        status_.latency.captureBufferFrames =
            capture ? toWireFrames(capture->bufferFrames(), inputRate) : 0;
        status_.latency.pcmMarginFrames =
            hasCapture ? toWireFrames(encoder.targetMarginFrames(), inputRate) : 0;
        status_.latency.encoderFrames = encoder.frameSamples();
        status_.latency.encoderDelayFrames = encoder.encoderDelayFrames();
        status_.latency.outputBufferFrames = output->bufferFrames();
    }
    setState(EngineState::running);

    while (!shouldStop()) {
        Sleep(200);
        const std::string lost = output->failure();
        if (!lost.empty()) {
            setState(EngineState::failed, lost);
            return;
        }
        const EncoderStats stats = encoder.stats();
        {
            std::lock_guard<std::mutex> guard(lock_);
            status_.secondsOnWire =
                output->renderedBytes() / output->bytesPerSecond();
            status_.bursts = output->renderedBytes() / config.burstBytes();
            status_.marginMs =
                hasCapture ? stats.marginFrames * 1000.0 / inputRate : 0;
            status_.driftPPM =
                static_cast<double>(stats.samplesDropped -
                                    stats.samplesDuplicated) *
                1000000.0 /
                static_cast<double>(stats.inputFramesConsumed > 0
                                        ? stats.inputFramesConsumed : 1);
            status_.dropouts = stats.dropouts;
            status_.silenceFrames = stats.silenceFrames;
            status_.encodeErrors = stats.encodeErrors;
            status_.encodeMsAverage = stats.encodeMsAverage;
            status_.encodeMsMax = stats.encodeMsMax;
            status_.captureOverflowFrames =
                capture ? capture->overflowFrames() : 0;
            if (capture) {
                status_.inputLevels = capture->levels();
                status_.inputPeakHold = capture->peakHold();
                status_.sourceChannels = capture->sourceChannelCount();
                status_.sourceChannelMask = capture->channelMask();
                // Once the controller has a measurement, report what the
                // margin actually is rather than what it was asked to be.
                if (stats.framesEncoded > 0 && stats.marginFrames > 0) {
                    status_.latency.pcmMarginFrames =
                        toWireFrames(stats.marginFrames, inputRate);
                }
            }
        }

        if (capture) warnAboutDiscardedChannels(capture->levels());
    }
}

// Audio arriving on a mix channel past the capture width is silently thrown
// away, which looks exactly like a dead speaker at the far end of the chain.
// Say so, once, rather than letting it be debugged from the receiver
// backwards.
void Engine::warnAboutDiscardedChannels(const std::vector<float>& levels) {
    int capturedChannels;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (warnedAboutDiscardedChannels_) return;
        capturedChannels = status_.capturedChannels;
    }
    if (static_cast<int>(levels.size()) <= capturedChannels) return;
    std::string discarded;
    for (size_t channel = capturedChannels; channel < levels.size(); ++channel) {
        if (levels[channel] > 0.01f) {
            if (!discarded.empty()) discarded += ", ";
            discarded += std::to_string(channel + 1);
        }
    }
    if (discarded.empty()) return;
    {
        std::lock_guard<std::mutex> guard(lock_);
        warnedAboutDiscardedChannels_ = true;
    }
    log("audio is present on source channel(s) " + discarded +
        " but capture is set to " + std::to_string(capturedChannels) +
        " channels, so it is being discarded. Raise the channel count to "
        "include it.");
}

}  // namespace ac3spdif
