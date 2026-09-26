// Device discovery and exclusive-format probing.
//
// Everything a bitstream needs to know about an endpoint is asked of the
// driver rather than guessed from the name: whether it accepts an IEC 61937
// format in exclusive mode is a question IsFormatSupported answers directly,
// and the form factor Windows reports (SPDIF, HDMI, Speakers…) says whether
// the connector is digital. That is a materially better position than macOS,
// where DTS has no stream format to advertise at all and terminal types are
// unreliable — here both codecs are probed for real, per device.

#pragma once

#include "Win32Util.h"

#include <mmdeviceapi.h>
#include <mmreg.h>

#include <string>
#include <vector>

namespace ac3spdif {

// Which codec the bitstream carries. Declared here rather than in the engine
// because format probing needs it: unlike macOS, Windows has a distinct
// IEC 61937 exclusive format for each codec, so "can this device bitstream"
// is a per-codec question.
enum class OutputCodec { ac3, dts };

// How the burst reaches the wire.
enum class CarrierMode {
    // An IEC 61937 exclusive-mode format (KSDATAFORMAT_SUBTYPE_IEC61937_*).
    // The audio engine is bypassed entirely and the driver raises the
    // IEC 60958 channel-status "non-audio" bit, so the receiver routes the
    // stream to its decoder rather than its DACs. Strongly preferred.
    encoded,
    // 16-bit exclusive LPCM carrying raw IEC 61937 bursts. Fallback for
    // devices whose driver rejects the encoded formats; the non-audio bit is
    // not set, so the receiver must detect the burst preamble itself — which
    // is how DTS audio CDs have always worked. Exclusive mode still bypasses
    // the mixer, so unlike the macOS PCM carrier the bytes are bit-exact by
    // construction: no float conversion, no dither risk.
    pcm,
};

struct DeviceInfo {
    // The endpoint ID string — unique and stable across reboots, unlike the
    // friendly name, so it is what settings store.
    std::wstring id;
    std::string name;
    bool isDefaultRender = false;

    // The shared-mode mix format, which is what loopback capture delivers.
    // Its channel count is set by the user's speaker configuration (Sound
    // settings → speaker setup), the exact analogue of BlackHole's
    // "Configure Speakers" step on macOS.
    int mixChannels = 0;
    double mixSampleRate = 0;
    DWORD mixChannelMask = 0;

    // Exclusive-mode format support, probed with IsFormatSupported rather
    // than inferred. Probed at the sample rate enumeration was asked for.
    bool supportsIEC61937AC3 = false;
    bool supportsIEC61937DTS = false;
    bool supportsPCM16 = false;

    // The endpoint form factor. SPDIF and HDMI are strong signals the
    // connector is digital; Speakers/Headphones mean an analog path where a
    // burst would be full-scale noise.
    int formFactor = 0;  // EndpointFormFactor
    bool isDigital = false;

    // The device's scheduling periods at the mix rate, in frames — Windows'
    // equivalent of the IOProc callback size, and what sets the smallest
    // output buffer the device can sustain. The minimum is what exclusive
    // event-driven mode can actually run at, typically a few milliseconds.
    int defaultPeriodFrames = 0;
    int minimumPeriodFrames = 0;

    bool supportsEncoded(OutputCodec codec) const {
        return codec == OutputCodec::ac3 ? supportsIEC61937AC3
                                         : supportsIEC61937DTS;
    }
};

std::string formFactorLabel(int formFactor);

// The exclusive-mode formats this project negotiates, built once and probed
// everywhere. Two shapes per codec because drivers disagree: most honour the
// modern WAVEFORMATEXTENSIBLE with an IEC 61937 subformat, but plenty of
// S/PDIF hardware only answers to the legacy tag (WAVE_FORMAT_DOLBY_AC3_SPDIF,
// a format old enough that it predates WAVEFORMATEXTENSIBLE entirely).
struct CandidateFormats {
    WAVEFORMATEXTENSIBLE extensible;
    WAVEFORMATEX legacy;
    bool hasLegacy;
};
CandidateFormats encodedFormats(OutputCodec codec, double sampleRate);
WAVEFORMATEXTENSIBLE pcmCarrierFormat(double sampleRate);
std::string formatDescription(const WAVEFORMATEX* format);

namespace AudioDevices {

// Every active render endpoint, with capabilities probed at `sampleRate`.
// Render endpoints serve both roles here: output is a render endpoint in
// exclusive mode, and capture is a render endpoint opened in loopback — the
// WASAPI feature that removes the need for a BlackHole-style virtual device.
std::vector<DeviceInfo> all(double sampleRate = 48000);

// Resolve a device from a user-supplied string: endpoint ID exactly first,
// then name exactly, then name case-insensitively by prefix, then substring.
// An empty query resolves to the default render device.
bool find(const std::string& query, double sampleRate, DeviceInfo& out);

// Best automatic choice for the bitstream sink.
//
// "First device that works" is the wrong answer on a machine with several
// candidates — a laptop with HDMI, a dock and a USB DAC will offer three, and
// enumeration order is not meaningful. Rank instead: a native IEC 61937
// format beats a PCM carrier by a wide margin, and a digital form factor
// beats one that is not. A PCM carrier on a device that does not report a
// digital form factor scores 1: indistinguishable from a headphone jack, and
// never chosen automatically — an AC-3 burst played into an analog output is
// full-scale digital noise, so that choice has to be made by hand.
struct RankedDevice {
    DeviceInfo device;
    int score;
};
std::vector<RankedDevice> rankedBitstreamSinks(double sampleRate,
                                               OutputCodec codec,
                                               bool allowPCMCarrier);

// What a device can do for a given codec, as a short label for menus and
// --list output.
std::string capability(const DeviceInfo& device, OutputCodec codec);

// The endpoint a given configuration would stream to, without claiming
// anything. Lets a UI show device-dependent facts — the latency floor, for
// one — before the engine has ever run.
bool previewOutput(const std::string& outputQuery, double sampleRate,
                   OutputCodec codec, DeviceInfo& out);

}  // namespace AudioDevices

}  // namespace ac3spdif
