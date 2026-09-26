// Everything the pipeline needs to know. Deliberately plain data with no
// parsing or UI logic attached: the CLI builds one from arguments, the tray
// app builds one from the registry, and neither knows about the other.

#pragma once

#include "../support/AudioDevices.h"

#include <string>
#include <vector>

namespace ac3spdif {

// The choice of codec changes the geometry of everything downstream. An AC-3
// frame is 1536 samples and rides in a 6144-byte IEC 61937 burst; a DTS Type I
// frame is 512 samples in a 2048-byte burst. The encoder cannot emit a frame
// until it has all of one, and a burst is the unit the receiver locks onto, so
// that number propagates through the whole pipeline. Nothing here may assume
// 1536.
namespace codec {

const char* name(OutputCodec codec);
// FFmpeg's name for the encoder. DTS is `dca`, after Coherent Acoustics.
const char* ffmpegEncoder(OutputCodec codec);
const char* shortName(OutputCodec codec);
bool parse(const std::string& text, OutputCodec& out);

// Samples per encoded frame, which is also frames per IEC 61937 burst.
inline int frameSamples(OutputCodec c) {
    return c == OutputCodec::ac3 ? 1536 : 512;
}
// The IEC 60958 carrier is always two 16-bit channels.
inline int burstBytes(OutputCodec c) { return frameSamples(c) * 4; }

// FFmpeg's DTS encoder is marked experimental and refuses to open without
// being told that is acceptable.
inline bool needsExperimentalFlag(OutputCodec c) { return c == OutputCodec::dts; }

// DTS accepts `5.1(side)` but not the back-channel `5.1`; AC-3 takes either.
// Stated explicitly so the encoder never has to guess.
std::string encodeLayout(OutputCodec codec, int channelCount);

// Bitrates the encoder will actually accept.
//
// These are not interchangeable between codecs and the overlap is empty:
// FFmpeg's DTS encoder refuses anything at or below 640k for 5.1 — which is
// every AC-3 setting — and fails to open rather than degrading, so a bitrate
// carried over from AC-3 stops the pipeline dead.
//
// AC-3 stops at 640k because A/52 does. DTS stops at 1509k because of the
// burst: a Type I burst is 2048 bytes with an 8-byte preamble, leaving 2040
// for payload; 1509k fills 2012 of those bytes, and 1536k needs exactly 2048,
// whereupon the muxer emits a stream with no IEC 61937 sync pattern in it at
// all — not a failure, just silently unusable, which is why it is not offered.
const std::vector<std::string>& supportedBitrates(OutputCodec codec);
bool acceptsBitrate(OutputCodec codec, const std::string& bitrate);
const char* defaultBitrate(OutputCodec codec);
// "640k" -> 640000. The strings are what menus and flags show; the encoder
// wants bits per second.
long long bitsPerSecond(const std::string& bitrate);

}  // namespace codec

const char* carrierLabel(CarrierMode mode);
const char* carrierName(CarrierMode mode);  // "iec61937" / "pcm"
bool parseCarrier(const std::string& text, CarrierMode& out);

// How much PCM to keep queued ahead of the encoder, traded against latency.
//
// This is the one latency knob worth exposing. The encoder runs on the output
// device's own render thread and encodes a frame only when the burst it is
// sending runs short, so there is no ring of encoded bursts anywhere: the only
// slack in the pipeline is this margin of not-yet-encoded PCM, which absorbs
// the jitter between the capture clock delivering packets and the output
// clock asking for a frame. Being PCM, it can be any number of samples — the
// old design had to hold whole 32 ms bursts, plus a spare one for the
// sawtooth of an encoder delivering a burst at a time.
enum class LatencyProfile { minimum, low, balanced, safe, maximum };

namespace latency_profile {
int bufferMilliseconds(LatencyProfile profile);
const char* name(LatencyProfile profile);
bool parse(const std::string& text, LatencyProfile& out);
inline const LatencyProfile all[] = {
    LatencyProfile::minimum, LatencyProfile::low, LatencyProfile::balanced,
    LatencyProfile::safe, LatencyProfile::maximum};
}  // namespace latency_profile

struct Config {
    // Endpoint to capture in loopback — a *render* endpoint, because WASAPI
    // loopback taps what Windows is playing to it. Empty means the default
    // output device, which is where system audio goes unless told otherwise.
    std::string inputQuery;
    // Empty = auto-select the best IEC 61937-capable device. "none" = encode
    // and discard, for diagnosing capture without claiming any device.
    std::string outputQuery;
    // Channels to take from the loopback mix. 0 = match the mix format, which
    // is set by the user's speaker configuration in Sound settings.
    int channelCount = 0;
    std::string explicitLayout;
    // Empty means "whatever suits the codec": 640k for AC-3, 1509k for DTS.
    std::string bitrate;
    double sampleRate = 48000;  // the wire rate; capture may differ and is resampled
    OutputCodec codec = OutputCodec::ac3;
    CarrierMode carrier = CarrierMode::encoded;
    // PCM safety margin ahead of the encoder, in milliseconds.
    int bufferMs = 30;
    // Ceiling on the drift correction, in samples added or removed per
    // 1536-sample block (scaled for other frame lengths and input rates so the
    // ppm ceiling is the same everywhere). 8 is about 5200 ppm.
    int maxDriftFrames = 8;
    double statusInterval = 5;
    bool quiet = false;
    // Print a per-channel input level meter. Purely diagnostic, and the
    // fastest way to find out which channels an application actually writes.
    bool showMeters = false;
    // Bypass capture entirely and synthesise a distinct tone per channel on
    // the render thread. Exercises the whole output path — exclusive mode,
    // format negotiation, render events, encoder, receiver handshake — with a
    // single clock and nothing to drift against.
    bool selfTest = false;
    // Tee the IEC 61937 stream to a file. Decoding that file with
    // `ffmpeg -f spdif -i <file>` reproduces exactly what went to the wire,
    // which is the only way to tell a capture/encode fault from a cabling or
    // receiver fault without an analyser.
    std::string dumpPath;

    // The requested bitrate if the codec can take it, otherwise the codec's
    // own default. Substituting beats refusing to start over a setting the
    // user probably chose for a different codec entirely.
    std::string effectiveBitrate() const {
        if (bitrate.empty() || !codec::acceptsBitrate(codec, bitrate)) {
            return codec::defaultBitrate(codec);
        }
        return bitrate;
    }
    bool bitrateWasSubstituted() const {
        return !bitrate.empty() && !codec::acceptsBitrate(codec, bitrate);
    }

    int frameSamples() const { return codec::frameSamples(codec); }
    int burstBytes() const { return codec::burstBytes(codec); }

    // AC-3 tops out at 5.1 — the codec has no 7.1 mode, and E-AC-3, which
    // does, is not something S/PDIF carries to most receivers. Capturing
    // eight channels is still worth doing when the source produces them:
    // folding the side pair into the back pair keeps information that simply
    // taking the first six channels would throw away.
    static constexpr int maxEncodedChannels = 6;

    std::string encodeLayout(int capturedChannels) const {
        const int encoded = capturedChannels < maxEncodedChannels
                                ? capturedChannels : maxEncodedChannels;
        return codec::encodeLayout(codec, encoded);
    }

    // Channels the self-test generator produces.
    int selfTestChannels() const { return channelCount > 0 ? channelCount : 6; }

    // AC-3 defines only these three rates.
    static const std::vector<double>& supportedSampleRates();
};

}  // namespace ac3spdif
