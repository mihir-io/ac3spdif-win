// audioprobe — dump every render endpoint, its mix format, its scheduling
// periods, and which exclusive-mode formats its driver actually accepts, with
// the bitstream-capable ones marked. Reach for it whenever something will not
// engage: the answers here are the driver's own, via IsFormatSupported, not
// inferences from names.

#include "../support/AudioDevices.h"
#include "../engine/Version.h"

#include <audioclient.h>
#include <wrl/client.h>

#include <cstdio>

using namespace ac3spdif;
using Microsoft::WRL::ComPtr;

static const char* mark(bool supported) { return supported ? "✓" : "✗"; }

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    ComInit com;
    std::printf("audioprobe %s (built %s)\n\n", kVersion, buildStamp());

    static const double rates[] = {32000, 44100, 48000};

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        std::fprintf(stderr, "cannot create the device enumerator\n");
        return 1;
    }

    for (const auto& device : AudioDevices::all(48000)) {
        std::printf("%s%s\n", device.name.c_str(),
                    device.isDefaultRender ? "   (default output)" : "");
        std::printf("  endpoint id   %s\n", narrow(device.id).c_str());
        std::printf("  form factor   %s%s\n",
                    formFactorLabel(device.formFactor).c_str(),
                    device.isDigital ? "  [digital]" : "");
        std::printf("  mix format    %d ch, %d Hz, mask 0x%03lX"
                    "   (what loopback capture delivers)\n",
                    device.mixChannels, static_cast<int>(device.mixSampleRate),
                    static_cast<unsigned long>(device.mixChannelMask));
        std::printf("  periods       default %d frames, minimum %d frames\n",
                    device.defaultPeriodFrames, device.minimumPeriodFrames);

        // Re-probe each rate with a fresh client; the cached DeviceInfo only
        // answers for 48 kHz.
        ComPtr<IMMDevice> mmDevice;
        if (FAILED(enumerator->GetDevice(device.id.c_str(), &mmDevice))) {
            std::printf("  (device disappeared mid-probe)\n\n");
            continue;
        }
        ComPtr<IAudioClient> client;
        if (FAILED(mmDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                      nullptr, &client))) {
            std::printf("  (no audio client)\n\n");
            continue;
        }

        std::printf("  exclusive-mode formats accepted:\n");
        for (const double rate : rates) {
            const CandidateFormats ac3 = encodedFormats(OutputCodec::ac3, rate);
            const CandidateFormats dts = encodedFormats(OutputCodec::dts, rate);
            const WAVEFORMATEXTENSIBLE pcm = pcmCarrierFormat(rate);
            const auto supported = [&](const WAVEFORMATEX* format) {
                return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                 format, nullptr) == S_OK;
            };
            std::printf("    %5d Hz   AC-3 %s (extensible) %s (legacy tag)   "
                        "DTS %s (extensible) %s (legacy tag)   PCM16 %s\n",
                        static_cast<int>(rate),
                        mark(supported(&ac3.extensible.Format)),
                        mark(supported(&ac3.legacy)),
                        mark(supported(&dts.extensible.Format)),
                        mark(supported(&dts.legacy)),
                        mark(supported(&pcm.Format)));
        }
        std::printf("\n");
    }

    std::printf(
        "An accepted IEC 61937 format means the driver will carry the burst\n"
        "with the non-audio bit set. It is not proof of a physical S/PDIF\n"
        "jack — the mapping from format to connector is the vendor's\n"
        "business. Verify at the receiver.\n");
    return 0;
}
