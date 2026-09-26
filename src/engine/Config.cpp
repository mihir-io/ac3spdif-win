#include "Config.h"

#include "../support/Win32Util.h"

#include <cstdlib>

namespace ac3spdif {

namespace codec {

const char* name(OutputCodec c) {
    return c == OutputCodec::ac3 ? "Dolby Digital (AC-3)" : "DTS";
}

const char* ffmpegEncoder(OutputCodec c) {
    return c == OutputCodec::ac3 ? "ac3" : "dca";
}

const char* shortName(OutputCodec c) {
    return c == OutputCodec::ac3 ? "ac3" : "dts";
}

bool parse(const std::string& text, OutputCodec& out) {
    const std::string low = lowered(text);
    if (low == "ac3") { out = OutputCodec::ac3; return true; }
    if (low == "dts") { out = OutputCodec::dts; return true; }
    return false;
}

std::string encodeLayout(OutputCodec c, int channelCount) {
    if (c == OutputCodec::dts && channelCount >= 6) return "5.1(side)";
    switch (channelCount) {
        case 6: return "5.1";
        case 2: return "stereo";
        case 1: return "mono";
        default: return std::to_string(channelCount) + "c";
    }
}

const std::vector<std::string>& supportedBitrates(OutputCodec c) {
    static const std::vector<std::string> ac3 = {"192k", "256k", "384k",
                                                 "448k", "640k"};
    static const std::vector<std::string> dts = {"754k", "1024k", "1280k",
                                                 "1411k", "1509k"};
    return c == OutputCodec::ac3 ? ac3 : dts;
}

bool acceptsBitrate(OutputCodec c, const std::string& bitrate) {
    for (const auto& rate : supportedBitrates(c)) {
        if (rate == bitrate) return true;
    }
    return false;
}

const char* defaultBitrate(OutputCodec c) {
    // For DTS, 1509k is the most the burst can hold; 1411k — the rate DTS
    // discs carry — is the safer choice for a receiver of unknown temperament
    // and remains on the list.
    return c == OutputCodec::ac3 ? "640k" : "1509k";
}

long long bitsPerSecond(const std::string& bitrate) {
    char* end = nullptr;
    const double value = std::strtod(bitrate.c_str(), &end);
    const double scale = (end && (*end == 'k' || *end == 'K')) ? 1000.0 : 1.0;
    return static_cast<long long>(value * scale + 0.5);
}

}  // namespace codec

const char* carrierLabel(CarrierMode mode) {
    return mode == CarrierMode::encoded
               ? "IEC 61937 (encoded, non-audio bit set)"
               : "PCM carrier (16-bit exclusive LPCM)";
}

const char* carrierName(CarrierMode mode) {
    return mode == CarrierMode::encoded ? "iec61937" : "pcm";
}

bool parseCarrier(const std::string& text, CarrierMode& out) {
    const std::string low = lowered(text);
    if (low == "iec61937" || low == "encoded" || low == "cac3") {
        out = CarrierMode::encoded;
        return true;
    }
    if (low == "pcm") { out = CarrierMode::pcm; return true; }
    return false;
}

namespace latency_profile {

int bufferMilliseconds(LatencyProfile profile) {
    // Measured on a 10 ms loopback period (see LatencyAdvice): the margin has
    // to cover one capture packet plus scheduling jitter, which puts the
    // floor in the low teens. The names are shorthands for numbers, nothing
    // more; --buffer takes any value.
    switch (profile) {
        case LatencyProfile::minimum: return 15;
        case LatencyProfile::low: return 20;
        case LatencyProfile::balanced: return 25;
        case LatencyProfile::safe: return 30;
        case LatencyProfile::maximum: return 80;
    }
    return 30;
}

const char* name(LatencyProfile profile) {
    switch (profile) {
        case LatencyProfile::minimum: return "minimum";
        case LatencyProfile::low: return "low";
        case LatencyProfile::balanced: return "balanced";
        case LatencyProfile::safe: return "safe";
        case LatencyProfile::maximum: return "maximum";
    }
    return "safe";
}

bool parse(const std::string& text, LatencyProfile& out) {
    const std::string low = lowered(text);
    for (const LatencyProfile profile : all) {
        if (low == name(profile)) {
            out = profile;
            return true;
        }
    }
    return false;
}

}  // namespace latency_profile

const std::vector<double>& Config::supportedSampleRates() {
    static const std::vector<double> rates = {32000, 44100, 48000};
    return rates;
}

}  // namespace ac3spdif
