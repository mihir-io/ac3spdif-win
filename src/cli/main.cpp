// The command-line front end. About two hundred lines over the same engine the
// tray app drives, so there is one pipeline implementation, not two that
// drift apart.

#include "../engine/Engine.h"
#include "../engine/LatencyAdvice.h"
#include "../engine/Version.h"
#include "CommandLine.h"

#include <cmath>
#include <cstdio>
#include <mutex>

using namespace ac3spdif;

static Engine* g_engine = nullptr;
static std::mutex g_printLock;

// Ctrl-C, console close and logoff all arrive here. The handler only signals;
// teardown runs on the engine's worker and main() waits for it, because the
// process must not exit while the device is still claimed. (Exclusive mode
// makes even that failure benign — Windows releases the endpoint when the
// process dies — but a clean stop also flushes the dump file.)
static BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT) {
        if (g_engine) g_engine->stop();
        if (signal == CTRL_CLOSE_EVENT && g_engine) {
            g_engine->waitUntilStopped(5);
        }
        return TRUE;
    }
    return FALSE;
}

static void printLine(const char* prefix, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_printLock);
    std::printf("%-8s: %s\n", prefix, message.c_str());
    std::fflush(stdout);
}

static void listDevices(const Config& config) {
    const auto devices = AudioDevices::all(config.sampleRate);
    std::printf("Render endpoints at %d Hz "
                "(output candidates, and loopback capture sources):\n\n",
                static_cast<int>(config.sampleRate));
    for (const auto& device : devices) {
        std::printf("  %s%s\n", device.name.c_str(),
                    device.isDefaultRender ? "   (default output)" : "");
        std::printf("      form factor  %s\n",
                    formFactorLabel(device.formFactor).c_str());
        std::printf("      mix format   %d ch, %d Hz  (what loopback captures)\n",
                    device.mixChannels,
                    static_cast<int>(device.mixSampleRate));
        std::printf("      periods      default %d frames, minimum %d frames\n",
                    device.defaultPeriodFrames, device.minimumPeriodFrames);
        std::printf("      AC-3         %s\n",
                    AudioDevices::capability(device, OutputCodec::ac3).c_str());
        std::printf("      DTS          %s\n",
                    AudioDevices::capability(device, OutputCodec::dts).c_str());
        std::printf("\n");
    }
    std::printf(
        "✓ IEC 61937 = the driver takes an encoded exclusive format and\n"
        "sets the non-audio bit. ~ PCM carrier = bursts ride in 16-bit LPCM\n"
        "and the receiver must detect them itself. Auto-selection only ever\n"
        "picks ✓ devices; a PCM carrier has to be named with --output.\n");
}

static std::string meterGlyph(float level) {
    static const char* glyphs[] = {"·", "▁", "▂", "▃",
                                   "▄", "▅", "▆", "▇",
                                   "█"};
    if (level <= 0.001f) return glyphs[0];
    // dBFS mapped over -50..0; a meter of raw amplitude never leaves the
    // bottom pixel for anything but full-scale content.
    const float db = 20.0f * std::log10(level);
    int step = static_cast<int>((db + 50.0f) / 50.0f * 8.0f) + 1;
    if (step < 1) step = 1;
    if (step > 8) step = 8;
    return glyphs[step];
}

static void printMeters(const EngineStatus& status) {
    std::string line;
    for (size_t channel = 0; channel < status.inputLevels.size(); ++channel) {
        char label[8];
        std::snprintf(label, sizeof(label), "%02zu", channel + 1);
        line += label;
        line += channel < static_cast<size_t>(status.capturedChannels) ? "" : "!";
        line += meterGlyph(status.inputLevels[channel]);
        line += " ";
    }
    printLine("meters", line);
}

static void printSummary(const EngineStatus& status) {
    if (status.inputPeakHold.empty()) return;
    printLine("summary", "peak level per source channel over the whole run");
    std::string line;
    bool anyDiscarded = false;
    for (size_t channel = 0; channel < status.inputPeakHold.size(); ++channel) {
        const bool discarded =
            channel >= static_cast<size_t>(status.capturedChannels);
        if (status.inputPeakHold[channel] > 0.001f && discarded) {
            anyDiscarded = true;
        }
        char entry[48];
        std::snprintf(entry, sizeof(entry), "  %02zu %s %.3f", channel + 1,
                      discarded ? "!" : " ", status.inputPeakHold[channel]);
        line += entry;
        if ((channel + 1) % 4 == 0 || channel + 1 == status.inputPeakHold.size()) {
            printLine("summary", line);
            line.clear();
        }
    }
    if (anyDiscarded) {
        printLine("summary",
                  "! = carried audio but was discarded; capture is set to " +
                      std::to_string(status.capturedChannels) +
                      " channels. Raise --channels to include it.");
    }
}

static void printLatencyBreakdown(const EngineStatus& status, double rate) {
    const LatencyBreakdown& l = status.latency;
    char line[128];
    const auto row = [&](const char* name, int frames, const char* note) {
        std::snprintf(line, sizeof(line), "%-18s %6d frames  %7.1f ms   %s",
                      name, frames, l.milliseconds(frames, rate), note);
        printLine("latency", line);
    };
    row("capture buffer", l.captureBufferFrames, "engine period, loopback");
    row("pcm margin", l.pcmMarginFrames, "what --buffer controls");
    row("encoder frame", l.encoderFrames, "inherent to the format");
    row("encoder delay", l.encoderDelayFrames, "codec look-ahead");
    row("output buffer", l.outputBufferFrames, "exclusive endpoint buffer");
    row("TOTAL", l.totalFrames(), "end to end");
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    ComInit com;

    ParsedArguments parsed;
    try {
        parsed = CLI::parse(argc, argv);
    } catch (const Failure& failure) {
        std::fprintf(stderr, "error   : %s\n", failure.what());
        std::fprintf(stderr, "          run with --help for usage\n");
        return 2;
    }
    if (parsed.help) {
        std::printf("%s", CLI::usage);
        return 0;
    }
    if (parsed.list) {
        try {
            listDevices(parsed.config);
        } catch (const Failure& failure) {
            std::fprintf(stderr, "error   : %s\n", failure.what());
            return 1;
        }
        return 0;
    }

    Engine engine(parsed.config);
    g_engine = &engine;
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    const bool quiet = parsed.config.quiet;
    engine.onLog = [quiet](const std::string& message) {
        if (!quiet) printLine("note", message);
    };
    engine.onStateChange = [&engine, quiet](EngineState state) {
        if (quiet && state != EngineState::failed) return;
        if (state == EngineState::failed) {
            printLine("error", engine.status().failureReason);
        } else {
            printLine("state", stateLabel(state));
        }
    };

    engine.start();

    bool printedRunningHeader = false;
    const ULONGLONG startTick = GetTickCount64();
    ULONGLONG lastStatusTick = startTick;
    ULONGLONG lastMeterTick = 0;
    for (;;) {
        Sleep(100);
        if (parsed.durationSeconds > 0 &&
            GetTickCount64() - startTick >=
                static_cast<ULONGLONG>(parsed.durationSeconds * 1000)) {
            engine.stop();
        }
        const EngineState state = engine.state();
        if (state == EngineState::idle || state == EngineState::failed) break;
        if (state != EngineState::running) continue;

        const EngineStatus status = engine.status();
        if (!printedRunningHeader) {
            printedRunningHeader = true;
            if (!quiet) {
                printLine("input", status.inputName);
                printLine("output", status.outputName + " as " + status.format);
                printLine("carrier", status.carrier);
                printLatencyBreakdown(status, parsed.config.sampleRate);
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (parsed.config.showMeters && now - lastMeterTick >= 500) {
            lastMeterTick = now;
            printMeters(status);
        }
        if (!quiet && parsed.config.statusInterval > 0 &&
            now - lastStatusTick >=
                static_cast<ULONGLONG>(parsed.config.statusInterval * 1000)) {
            lastStatusTick = now;
            char line[256];
            std::snprintf(line, sizeof(line),
                          "%.1f s on the wire | %lld bursts | margin %.1f/%d ms | "
                          "drift %+.0f ppm | dropouts %lld | encode %.2f/%.2f ms | "
                          "overflow %lld | silence %lld",
                          status.secondsOnWire,
                          static_cast<long long>(status.bursts),
                          status.marginMs, status.targetMarginMs,
                          status.driftPPM,
                          static_cast<long long>(status.dropouts),
                          status.encodeMsAverage, status.encodeMsMax,
                          static_cast<long long>(status.captureOverflowFrames),
                          static_cast<long long>(status.silenceFrames));
            printLine("status", line);
        }
    }

    // The breakdown printed at start-up has the target margin in it; by now
    // the controller has measured the real one.
    const EngineStatus lastRunning = engine.status();
    engine.waitUntilStopped(8);
    const EngineStatus finalStatus = engine.status();
    if (!quiet && printedRunningHeader) {
        printLine("latency", "measured over the run:");
        printLatencyBreakdown(lastRunning, parsed.config.sampleRate);
    }
    if (parsed.config.showMeters) printSummary(finalStatus);
    g_engine = nullptr;
    return finalStatus.state == EngineState::failed ? 1 : 0;
}
