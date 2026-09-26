// Engine — owns the pipeline's lifecycle.
//
//   default output ──loopback──▶ pcmRing ──▶ [drift · resample · encode · IEC 61937] ──▶ S/PDIF
//   (render clock A)              margin      on the output's render thread           (render clock B)
//
// The encoder runs on the output device's render thread and is pulled by it,
// so the PCM ring is the only buffer between the two clocks, and its margin
// is the only slack in the pipeline.
//
// Start-up and teardown happen on a worker thread, never the caller's. A tray
// app cannot afford to block its message loop for the second or so exclusive
// negotiation can take, and the CLI does not care either way. Callers observe
// progress through `onStateChange` and poll `status()`.
//
// Callbacks are invoked on the worker thread. A UI that needs them on its own
// thread marshals them itself (the tray app posts a message); the CLI prints
// from wherever it is called.

#pragma once

#include "../support/ByteRing.h"
#include "../support/Win32Util.h"
#include "BitstreamOutput.h"
#include "Capture.h"
#include "Config.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ac3spdif {

enum class EngineState { idle, starting, running, stopping, failed };

inline bool isActive(EngineState state) {
    return state == EngineState::starting || state == EngineState::running ||
           state == EngineState::stopping;
}

inline const char* stateLabel(EngineState state) {
    switch (state) {
        case EngineState::idle: return "Stopped";
        case EngineState::starting: return "Starting…";
        case EngineState::running: return "Streaming";
        case EngineState::stopping: return "Stopping…";
        case EngineState::failed: return "Failed";
    }
    return "";
}

// Live account of where the pipeline's delay is, measured rather than
// estimated: every term is a device's own reported buffer size, a reading
// from the encoder, or the measured margin. All in wire-rate frames.
struct LatencyBreakdown {
    // The capture engine period — what loopback delivers in one gulp.
    int captureBufferFrames = 0;
    // PCM left queued at the trough of each frame — what --buffer controls.
    int pcmMarginFrames = 0;
    // The encoder cannot emit a frame until it has all of one. Inherent to
    // the format; the only term here that no setting can touch.
    int encoderFrames = 0;
    // The encoder's own look-ahead, as libavcodec reports it.
    int encoderDelayFrames = 0;
    // The exclusive endpoint buffer: one device period.
    int outputBufferFrames = 0;

    int totalFrames() const {
        return captureBufferFrames + pcmMarginFrames + encoderFrames +
               encoderDelayFrames + outputBufferFrames;
    }
    double milliseconds(int frames, double sampleRate) const {
        return sampleRate > 0 ? frames / sampleRate * 1000 : 0;
    }
    double totalMilliseconds(double sampleRate) const {
        return milliseconds(totalFrames(), sampleRate);
    }
};

struct EngineStatus {
    EngineState state = EngineState::idle;
    std::string failureReason;
    std::string inputName;
    std::string outputName;
    std::string carrier;
    std::string format;
    double secondsOnWire = 0;
    int64_t bursts = 0;
    // The measured PCM margin at its trough, and what it is steered toward.
    double marginMs = 0;
    int targetMarginMs = 0;
    double driftPPM = 0;
    // Gaps in audio that was otherwise playing — the figure that says the
    // margin is too small for this machine.
    int64_t dropouts = 0;
    // Frames sent as encoded silence: nothing playing, and dropouts.
    int64_t silenceFrames = 0;
    int64_t encodeErrors = 0;
    // Time spent encoding a frame on the render thread, which has one
    // device period to spare.
    double encodeMsAverage = 0;
    double encodeMsMax = 0;
    int64_t captureOverflowFrames = 0;
    // Peak level per channel the capture mix presents, 0...1. Longer than the
    // captured channel count when the mix offers more.
    std::vector<float> inputLevels;
    int sourceChannels = 0;
    int capturedChannels = 0;
    // The mix format's speaker mask. Channel order in a WASAPI stream follows
    // the mask's bit order, so this is what turns "channel 3" into "LFE" in a
    // meter — the difference between a label and a puzzle.
    uint32_t sourceChannelMask = 0;
    // Highest level seen per channel over the whole run, never decayed.
    std::vector<float> inputPeakHold;
    LatencyBreakdown latency;
};

class Engine {
public:
    explicit Engine(const Config& config);
    ~Engine();

    const Config config;

    // Invoked on the worker thread; see the header comment.
    std::function<void(EngineState)> onStateChange;
    std::function<void(const std::string&)> onLog;

    // Returns immediately. Watch onStateChange for the outcome.
    void start();
    // Signals the worker and returns immediately; teardown finishes on the
    // worker thread and lands as a final idle state change.
    void stop();
    // For callers that must not exit until the device is released — the CLI
    // on Ctrl-C, and the app on quit.
    bool waitUntilStopped(double timeoutSeconds);

    EngineStatus status() const;
    EngineState state() const;

    // Per-channel input levels read straight from the capture, bypassing the
    // status snapshot. The snapshot refreshes five times a second, which is
    // ample for margin and drift figures but makes a level meter visibly step
    // rather than move; meters read this instead.
    std::vector<float> liveInputLevels() const;

private:
    void run();
    void startPipeline();
    void setState(EngineState state, const std::string& reason = "");
    void log(const std::string& message);
    bool shouldStop() const;
    void warnAboutDiscardedChannels(const std::vector<float>& levels);

    mutable std::mutex lock_;
    EngineStatus status_;
    bool stopRequested_ = false;
    std::thread worker_;
    Handle finished_;
    bool warnedAboutDiscardedChannels_ = false;
    // Held so meters can be read without waiting for the status snapshot.
    Capture* activeCapture_ = nullptr;
};

}  // namespace ac3spdif
