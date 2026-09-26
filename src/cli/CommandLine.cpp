#include "CommandLine.h"

#include "../engine/Version.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ac3spdif {
namespace CLI {

const char* usage = R"(ac3spdif — real-time Dolby Digital Live for Windows

Captures system audio with WASAPI loopback (no virtual device needed), encodes
it to AC-3 or DTS in-process with libavcodec on the output device's own render
thread, and bitstreams the result over S/PDIF as an IEC 61937 data burst
through a WASAPI exclusive-mode stream.

USAGE
  ac3spdif [options]
  ac3spdif --list

OPTIONS
  --input <name|id>     Render endpoint to capture in loopback
                        (default: the Windows default output device)
  --output <name|id>    S/PDIF device      (default: best IEC 61937-capable device)
  --output none         Encode and discard - diagnose capture, claim no device
  --channels <n>        Channels to capture (default: match the mix format;
                        encoded as at most 5.1)
  --layout <layout>     ffmpeg channel layout (default: derived from the mix)
  --codec <name>        ac3 | dts          (default: ac3)
  --bitrate <rate>      Encoder bitrate    (default: per codec)
                        AC-3 and DTS accept different values and the sets do
                        not overlap; an unusable one is replaced with the
                        codec default rather than failing to start.
  --carrier <mode>      iec61937 | pcm     (default: iec61937)
  --rate <hz>           Wire sample rate   (default: 48000)
  --buffer <ms>         PCM margin ahead of the encoder (default: 30)
  --latency <profile>   minimum | low | balanced | safe | maximum
  --max-drift <frames>  Max drift correction (default: 8 = ~5200 ppm)
  --status <seconds>    Status line interval (default: 5, 0 to disable)
  --quiet               Suppress status lines
  --meters              Show a per-channel input level meter
  --selftest            Emit a distinct tone per channel instead of capturing
  --dump <path>         Also write the IEC 61937 stream to a file
  --duration <seconds>  Stop cleanly after this long (for scripted runs)
  --list                List devices and their bitstream capability
  --version             Print the version and exit
  --help                This text

CAPTURE
  There is no loopback driver to install: WASAPI taps any render endpoint for
  exactly what Windows mixes to it. The mix format follows the device's
  speaker configuration, so for 5.1 capture open Sound settings, pick the
  capture device, and set its speaker setup to 5.1 — the same job BlackHole's
  "Configure Speakers" did on macOS, minus the third-party kernel driver.

  The capture source and the bitstream output cannot be the same endpoint:
  loopback taps the shared mixer and exclusive mode shuts that mixer out. Set
  the Windows default output to the device you want to listen "through" (the
  TV, the speakers) and point --output at the S/PDIF interface.

DIAGNOSING A SILENT SPEAKER
  --meters prints one level bar per channel the mix presents, not just the
  ones being encoded. A channel marked ! is beyond the capture width and is
  being discarded, so signal appearing there is the answer to "why is that
  speaker silent". The summary printed on exit is the undecayed record of the
  whole run, so nothing has to be caught live.

LATENCY
  The encoder runs on the output device's render thread and encodes a frame
  only when the burst it is sending runs short, so there is no buffer of
  encoded bursts anywhere. The only slack is a margin of PCM queued ahead of
  the encoder, and --buffer sets it in milliseconds, to the sample. It has to
  cover one loopback capture period (10 ms on every device measured) plus
  scheduling jitter; the engine warns when it is below the floor it works
  out for the device in use.

    minimum    15 ms
    low        20 ms
    balanced   25 ms
    safe       30 ms   default
    maximum    80 ms

  Watch the dropouts figure in the status line: if it climbs while audio is
  playing, the margin is too small for this machine. Clock drift between the
  capture and output devices is corrected in both directions, one sample at a
  time, so a slow capture clock is padded rather than left to run dry.

CODECS
  ac3   Dolby Digital. 1536 samples per frame in a 6144-byte IEC 61937
        burst, 32 ms.
  dts   DTS Coherent Acoustics, via FFmpeg's experimental encoder. 512
        samples per frame in a 2048-byte burst, 10.7 ms — the frame is the
        largest fixed term in the latency budget, so DTS takes about 21 ms
        off it. Unlike macOS, Windows defines an IEC 61937 exclusive format
        for DTS, so it gets the non-audio bit and a first-class carrier
        rather than having to hide in PCM.

CARRIER MODES
  iec61937  An exclusive-mode KSDATAFORMAT_SUBTYPE_IEC61937_* format. The
            audio engine is bypassed and the driver raises the IEC 60958
            non-audio bit, so the receiver routes the stream to its decoder.
            Use this whenever the device offers it.
  pcm       Exclusive 16-bit LPCM carrying the same bytes. For devices whose
            driver rejects the encoded formats; the non-audio bit is not set,
            so the receiver must detect the burst preamble itself. Exclusive
            mode still bypasses the mixer, so the bytes are bit-exact.
)";

ParsedArguments CLI::parse(int argc, wchar_t** argv) {
    ParsedArguments parsed;
    Config& config = parsed.config;

    int index = 1;
    auto next = [&](const std::string& flag) -> std::string {
        if (++index >= argc) throw Failure(flag + " needs a value");
        return narrow(argv[index]);
    };

    for (; index < argc; ++index) {
        const std::string argument = narrow(argv[index]);
        if (argument == "--input") config.inputQuery = next(argument);
        else if (argument == "--output") config.outputQuery = next(argument);
        else if (argument == "--channels") {
            const int value = std::atoi(next(argument).c_str());
            if (value < 1 || value > 16) throw Failure("--channels must be 1...16");
            config.channelCount = value;
        } else if (argument == "--layout") config.explicitLayout = next(argument);
        else if (argument == "--bitrate") config.bitrate = next(argument);
        else if (argument == "--codec") {
            if (!codec::parse(next(argument), config.codec)) {
                throw Failure("--codec must be 'ac3' or 'dts'");
            }
        } else if (argument == "--rate") {
            const double value = std::atof(next(argument).c_str());
            bool supported = false;
            for (const double rate : Config::supportedSampleRates()) {
                if (rate == value) supported = true;
            }
            if (!supported) {
                throw Failure("--rate must be 32000, 44100 or 48000 "
                              "(AC-3 only defines these)");
            }
            config.sampleRate = value;
        } else if (argument == "--dump") config.dumpPath = next(argument);
        else if (argument == "--duration") {
            parsed.durationSeconds = std::atof(next(argument).c_str());
            if (parsed.durationSeconds <= 0) {
                throw Failure("--duration needs a positive number of seconds");
            }
        }
        else if (argument == "--carrier") {
            if (!parseCarrier(next(argument), config.carrier)) {
                throw Failure("--carrier must be 'iec61937' or 'pcm'");
            }
        } else if (argument == "--latency") {
            LatencyProfile profile;
            if (!latency_profile::parse(next(argument), profile)) {
                throw Failure("--latency must be one of: minimum, low, "
                              "balanced, safe, maximum");
            }
            config.bufferMs = latency_profile::bufferMilliseconds(profile);
        } else if (argument == "--buffer") {
            const int value = std::atoi(next(argument).c_str());
            if (value < 1 || value > 200) throw Failure("--buffer must be 1...200 ms");
            config.bufferMs = value;
        } else if (argument == "--max-drift") {
            const int value = std::atoi(next(argument).c_str());
            if (value < 1 || value > 16) throw Failure("--max-drift must be 1...16");
            config.maxDriftFrames = value;
        } else if (argument == "--status") {
            const double value = std::atof(next(argument).c_str());
            if (value < 0) throw Failure("--status needs a non-negative number");
            config.statusInterval = value;
        } else if (argument == "--quiet") config.quiet = true;
        else if (argument == "--meters") config.showMeters = true;
        else if (argument == "--selftest") config.selfTest = true;
        else if (argument == "--list") parsed.list = true;
        else if (argument == "--version") {
            std::printf("ac3spdif %s (built %s)\n", kVersion, buildStamp());
            std::exit(0);
        } else if (argument == "--help" || argument == "-h") parsed.help = true;
        else throw Failure("unknown option " + argument);
    }
    return parsed;
}

}  // namespace CLI
}  // namespace ac3spdif
