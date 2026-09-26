// How small the PCM margin can sensibly go, for a given pair of devices.
//
// The margin is the PCM queued ahead of the encoder at the moment the output
// thread takes a frame. Starvation happens when a frame is due and its last
// samples have not arrived yet, so the margin has to cover how late they can
// be. Three things set that, and they add:
//
//   The capture packet. Loopback delivers audio in one engine period at a
//   time — 10 ms on every endpoint measured — so the newest samples can be
//   up to one period behind the ones already queued.
//
//   Scheduling jitter. The capture thread drains on a 2 ms timer, and
//   neither it nor the render thread wakes at exactly the instant it asks
//   for. A few milliseconds covers both under MMCSS.
//
//   The render gulp. One render event can need more than one frame encoded
//   at once when the device period is longer than a burst — a 4096-frame
//   period wants nearly three AC-3 frames per event — and every frame past
//   the first has to be queued already.
//
// None of this depends on the codec except through the gulp, which is the
// point of keeping the slack in PCM: the old burst ring needed four DTS
// bursts where two AC-3 bursts sufficed, because its unit was the burst.

#pragma once

#include "../support/AudioDevices.h"
#include "Config.h"

#include <cmath>

namespace ac3spdif {
namespace LatencyAdvice {

inline constexpr double schedulingJitterMs = 5.0;
inline constexpr int maximumBufferMs = 200;

// The smallest margin, in milliseconds, that capture on `input` feeding output
// on `output` can be expected to sustain. Either may be null (self-test has no
// capture; --output none has no device period).
inline int minimumBufferMs(const DeviceInfo* input, const DeviceInfo* output,
                           OutputCodec codec, double sampleRate = 48000) {
    if (!input) return 0;
    double captureMs = 10.0;
    if (input->defaultPeriodFrames > 0 && input->mixSampleRate > 0) {
        captureMs = input->defaultPeriodFrames * 1000.0 / input->mixSampleRate;
    }
    double gulpMs = 0;
    if (output && output->defaultPeriodFrames > 0) {
        const int periodBytes = output->defaultPeriodFrames * 4;
        const int burst = codec::burstBytes(codec);
        const int framesPerEvent = (periodBytes + burst - 1) / burst;
        if (framesPerEvent > 1) {
            gulpMs = (framesPerEvent - 1) * codec::frameSamples(codec) * 1000.0 /
                     sampleRate;
        }
    }
    return static_cast<int>(std::ceil(captureMs + schedulingJitterMs + gulpMs));
}

// Milliseconds a whole encoded frame spans on the wire.
inline double frameMilliseconds(OutputCodec codec, double sampleRate) {
    return sampleRate > 0 ? codec::frameSamples(codec) * 1000.0 / sampleRate : 0;
}

}  // namespace LatencyAdvice
}  // namespace ac3spdif
