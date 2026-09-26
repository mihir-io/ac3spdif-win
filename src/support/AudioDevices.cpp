#include "AudioDevices.h"

#include <audioclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace ac3spdif {

// The GUIDs and property keys used here, spelled out rather than pulled from
// ksmedia.h and functiondiscoverykeys_devpkey.h, both of which require INITGUID
// gymnastics in exactly one translation unit to link. The IEC 61937 subformats
// are the wave format tag zero-extended into the standard audio GUID range, so
// the values are visible at a glance: 0x92 is WAVE_FORMAT_DOLBY_AC3_SPDIF and
// 0x08 is WAVE_FORMAT_DTS.
static const GUID kSubformatIEC61937DolbyDigital = {
    0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubformatIEC61937DTS = {
    0x00000008, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubformatPCM = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubformatIEEEFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static const PROPERTYKEY kPKeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};
static const PROPERTYKEY kPKeyFormFactor = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}},
    0};

#ifndef WAVE_FORMAT_DOLBY_AC3_SPDIF
#define WAVE_FORMAT_DOLBY_AC3_SPDIF 0x0092
#endif
#ifndef WAVE_FORMAT_DTS_LEGACY
#define WAVE_FORMAT_DTS_LEGACY 0x0008
#endif

std::string formFactorLabel(int formFactor) {
    switch (formFactor) {
        case 1: return "Speakers";
        case 2: return "Line";
        case 3: return "Headphones";
        case 5: return "Headset";
        case 7: return "Digital passthrough";
        case 8: return "S/PDIF";
        case 9: return "HDMI/DisplayPort";
        default: return "unknown";
    }
}

static bool isDigitalFormFactor(int formFactor) {
    // SPDIF, DigitalAudioDisplayDevice (HDMI/DP) and UnknownDigitalPassthrough.
    return formFactor == 7 || formFactor == 8 || formFactor == 9;
}

CandidateFormats encodedFormats(OutputCodec codec, double sampleRate) {
    CandidateFormats out{};

    // The IEC 60958 carrier is always two 16-bit subframes, whatever surround
    // payload rides inside the burst. Do not read the "2 channels" as a
    // channel count: a 2ch IEC 61937 stream carries full 5.1 perfectly well.
    WAVEFORMATEXTENSIBLE& x = out.extensible;
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels = 2;
    x.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    x.Format.wBitsPerSample = 16;
    x.Format.nBlockAlign = 4;
    x.Format.nAvgBytesPerSec = x.Format.nSamplesPerSec * x.Format.nBlockAlign;
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample = 16;
    x.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    x.SubFormat = codec == OutputCodec::ac3 ? kSubformatIEC61937DolbyDigital
                                            : kSubformatIEC61937DTS;

    // The pre-WAVEFORMATEXTENSIBLE shape. Old S/PDIF drivers, and some current
    // ones behind them in spirit, only answer to this.
    WAVEFORMATEX& l = out.legacy;
    l.wFormatTag = codec == OutputCodec::ac3
                       ? WAVE_FORMAT_DOLBY_AC3_SPDIF
                       : WAVE_FORMAT_DTS_LEGACY;
    l.nChannels = 2;
    l.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    l.wBitsPerSample = 16;
    l.nBlockAlign = 4;
    l.nAvgBytesPerSec = l.nSamplesPerSec * l.nBlockAlign;
    l.cbSize = 0;
    out.hasLegacy = true;
    return out;
}

WAVEFORMATEXTENSIBLE pcmCarrierFormat(double sampleRate) {
    WAVEFORMATEXTENSIBLE x{};
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels = 2;
    x.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    x.Format.wBitsPerSample = 16;
    x.Format.nBlockAlign = 4;
    x.Format.nAvgBytesPerSec = x.Format.nSamplesPerSec * x.Format.nBlockAlign;
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample = 16;
    x.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    x.SubFormat = kSubformatPCM;
    return x;
}

std::string formatDescription(const WAVEFORMATEX* format) {
    if (!format) return "(none)";
    const WORD tag = format->wFormatTag;
    std::string kind;
    if (tag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (x->SubFormat == kSubformatIEC61937DolbyDigital) kind = "IEC 61937 AC-3";
        else if (x->SubFormat == kSubformatIEC61937DTS) kind = "IEC 61937 DTS";
        else if (x->SubFormat == kSubformatPCM) kind = "PCM";
        else if (x->SubFormat == kSubformatIEEEFloat) kind = "float";
        else kind = "extensible";
    } else if (tag == WAVE_FORMAT_DOLBY_AC3_SPDIF) {
        kind = "AC-3 over S/PDIF (legacy tag)";
    } else if (tag == WAVE_FORMAT_DTS_LEGACY) {
        kind = "DTS (legacy tag)";
    } else if (tag == WAVE_FORMAT_PCM) {
        kind = "PCM";
    } else if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        kind = "float";
    } else {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "tag 0x%04X", tag);
        kind = buffer;
    }
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%lu Hz %s %uch %u-bit",
                  static_cast<unsigned long>(format->nSamplesPerSec),
                  kind.c_str(), format->nChannels, format->wBitsPerSample);
    return buffer;
}

namespace AudioDevices {

static ComPtr<IMMDeviceEnumerator> makeEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    throwIfFailed(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "cannot create the audio device enumerator (is COM initialized?)");
    return enumerator;
}

static bool exclusiveFormatSupported(IAudioClient* client,
                                     const WAVEFORMATEX* format) {
    // In exclusive mode there is no "closest match": the driver either takes
    // the format or it does not.
    return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format,
                                     nullptr) == S_OK;
}

static bool describe(IMMDevice* device, const std::wstring& defaultId,
                     double sampleRate, DeviceInfo& info) {
    LPWSTR rawId = nullptr;
    if (FAILED(device->GetId(&rawId)) || !rawId) return false;
    info.id = rawId;
    CoTaskMemFree(rawId);
    info.isDefaultRender = info.id == defaultId;

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(kPKeyFriendlyName, &value)) &&
            value.vt == VT_LPWSTR) {
            info.name = narrow(value.pwszVal);
        }
        PropVariantClear(&value);
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(kPKeyFormFactor, &value)) &&
            value.vt == VT_UI4) {
            info.formFactor = static_cast<int>(value.ulVal);
        }
        PropVariantClear(&value);
    }
    if (info.name.empty()) info.name = narrow(info.id);
    info.isDigital = isDigitalFormFactor(info.formFactor);

    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                &client))) {
        // A device that cannot even hand out an IAudioClient (mid-unplug,
        // driver trouble) is not a candidate for anything.
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        info.mixChannels = mix->nChannels;
        info.mixSampleRate = mix->nSamplesPerSec;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            info.mixChannelMask =
                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        }
        CoTaskMemFree(mix);
    }

    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
        const double rate = info.mixSampleRate > 0 ? info.mixSampleRate : 48000;
        info.defaultPeriodFrames =
            static_cast<int>(defaultPeriod * rate / 10000000.0 + 0.5);
        info.minimumPeriodFrames =
            static_cast<int>(minimumPeriod * rate / 10000000.0 + 0.5);
    }

    const CandidateFormats ac3 = encodedFormats(OutputCodec::ac3, sampleRate);
    const CandidateFormats dts = encodedFormats(OutputCodec::dts, sampleRate);
    const WAVEFORMATEXTENSIBLE pcm = pcmCarrierFormat(sampleRate);
    info.supportsIEC61937AC3 =
        exclusiveFormatSupported(client.Get(), &ac3.extensible.Format) ||
        exclusiveFormatSupported(client.Get(), &ac3.legacy);
    info.supportsIEC61937DTS =
        exclusiveFormatSupported(client.Get(), &dts.extensible.Format) ||
        exclusiveFormatSupported(client.Get(), &dts.legacy);
    info.supportsPCM16 = exclusiveFormatSupported(client.Get(), &pcm.Format);
    return true;
}

std::vector<DeviceInfo> all(double sampleRate) {
    auto enumerator = makeEnumerator();

    std::wstring defaultId;
    {
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                          &defaultDevice))) {
            LPWSTR rawId = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&rawId)) && rawId) {
                defaultId = rawId;
                CoTaskMemFree(rawId);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    throwIfFailed(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                 &collection),
                  "cannot enumerate audio endpoints");
    UINT count = 0;
    collection->GetCount(&count);

    std::vector<DeviceInfo> devices;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) continue;
        DeviceInfo info;
        if (describe(device.Get(), defaultId, sampleRate, info)) {
            devices.push_back(std::move(info));
        }
    }
    return devices;
}

bool find(const std::string& query, double sampleRate, DeviceInfo& out) {
    const auto devices = all(sampleRate);
    if (query.empty()) {
        for (const auto& device : devices) {
            if (device.isDefaultRender) {
                out = device;
                return true;
            }
        }
        return false;
    }
    const std::wstring wideQuery = widen(query);
    for (const auto& device : devices) {
        if (device.id == wideQuery) { out = device; return true; }
    }
    for (const auto& device : devices) {
        if (device.name == query) { out = device; return true; }
    }
    const std::string low = lowered(query);
    for (const auto& device : devices) {
        if (lowered(device.name).rfind(low, 0) == 0) { out = device; return true; }
    }
    for (const auto& device : devices) {
        if (lowered(device.name).find(low) != std::string::npos) {
            out = device;
            return true;
        }
    }
    return false;
}

std::vector<RankedDevice> rankedBitstreamSinks(double sampleRate,
                                               OutputCodec codec,
                                               bool allowPCMCarrier) {
    std::vector<RankedDevice> ranked;
    for (const auto& device : all(sampleRate)) {
        int score = 0;
        if (device.supportsEncoded(codec)) {
            score = 100 + (device.isDigital ? 10 : 0);
        } else if (allowPCMCarrier && device.supportsPCM16) {
            score = device.isDigital ? 50 : 1;
        } else {
            continue;
        }
        ranked.push_back({device, score});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const RankedDevice& a, const RankedDevice& b) {
                  return a.score > b.score;
              });
    return ranked;
}

std::string capability(const DeviceInfo& device, OutputCodec codec) {
    if (device.supportsEncoded(codec)) return "✓ IEC 61937";
    if (device.supportsPCM16) return "~ PCM carrier only";
    return "✗ unsupported";
}

bool previewOutput(const std::string& outputQuery, double sampleRate,
                   OutputCodec codec, DeviceInfo& out) {
    if (lowered(outputQuery) == "none") return false;
    if (!outputQuery.empty()) return find(outputQuery, sampleRate, out);
    const auto ranked = rankedBitstreamSinks(sampleRate, codec, false);
    if (ranked.empty()) return false;
    out = ranked.front().device;
    return true;
}

}  // namespace AudioDevices

}  // namespace ac3spdif
