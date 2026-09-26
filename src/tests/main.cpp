// ac3spdif_tests — the checks that do not need hardware.
//
//   round trip   Self-test tones through the in-process encoder, framed as
//                IEC 61937, then decoded back with libavformat's spdif demuxer.
//                Every burst must open with the Pa/Pb sync words, and a
//                Goertzel filter per decoded channel must find that channel's
//                own pitch — the same proof the macOS project used on
//                hardware, so channel order is checked, not just "it decodes".
//
//   drift        The encoder pulled at the wire rate by a simulated output
//                clock while a simulated capture clock runs fast or slow. The
//                controller must converge on its target margin, report the
//                clock error it is correcting, and never drop out.
//
//   starvation   Playback stopping is idle, not a dropout; a capture stall
//                that starves the encoder mid-playback is exactly one dropout.
//
// Simulated time throughout, so minutes of clock drift run in seconds.

#include "../engine/Config.h"
#include "../engine/Encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace ac3spdif;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition, ...)                                              \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(condition)) {                                                \
            ++g_failures;                                                  \
            std::printf("  FAIL  %s:%d  %s — ", __FILE__, __LINE__, #condition); \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

static double goertzelPower(const std::vector<float>& samples, size_t start,
                            size_t count, double frequency, double rate) {
    const double coefficient = 2.0 * std::cos(2.0 * 3.14159265358979 * frequency / rate);
    double previous = 0, previous2 = 0;
    for (size_t i = start; i < start + count && i < samples.size(); ++i) {
        const double current = samples[i] + coefficient * previous - previous2;
        previous2 = previous;
        previous = current;
    }
    return previous * previous + previous2 * previous2 -
           coefficient * previous * previous2;
}

static float sampleAt(const AVFrame* frame, int channel, int index) {
    const int channels = frame->ch_layout.nb_channels;
    const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format));
    const int plane = planar ? channel : 0;
    const int offset = planar ? index : index * channels + channel;
    switch (frame->format) {
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            return reinterpret_cast<const float*>(frame->extended_data[plane])[offset];
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            return reinterpret_cast<const int32_t*>(frame->extended_data[plane])[offset] /
                   2147483648.0f;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            return reinterpret_cast<const int16_t*>(frame->extended_data[plane])[offset] /
                   32768.0f;
        default:
            return 0;
    }
}

// The pitch each surround position carries in the self-test signal. Keyed by
// position rather than index, because AC-3 and DTS decode to different
// channel orders and 5.1 back becomes 5.1 side on the way into DTS.
static double expectedPitch(AVChannel channel) {
    switch (channel) {
        case AV_CHAN_FRONT_LEFT: return 440;
        case AV_CHAN_FRONT_RIGHT: return 554;
        case AV_CHAN_FRONT_CENTER: return 659;
        case AV_CHAN_LOW_FREQUENCY: return 82;
        case AV_CHAN_BACK_LEFT:
        case AV_CHAN_SIDE_LEFT: return 880;
        case AV_CHAN_BACK_RIGHT:
        case AV_CHAN_SIDE_RIGHT: return 1046;
        default: return 0;
    }
}

static void testRoundTrip(OutputCodec which) {
    std::printf("round trip: %s\n", codec::name(which));
    Config config;
    config.codec = which;
    config.selfTest = true;
    Encoder encoder(config, config.sampleRate, 6, 0, nullptr);
    encoder.open();

    const size_t burst = static_cast<size_t>(config.burstBytes());
    const size_t totalBytes = static_cast<size_t>(3 * config.sampleRate) * 4;
    std::vector<uint8_t> stream(totalBytes);
    // Pull in device-period pieces, the way the render thread does.
    for (size_t offset = 0; offset < totalBytes; offset += 1920) {
        const size_t piece = totalBytes - offset < 1920 ? totalBytes - offset : 1920;
        encoder.pull(stream.data() + offset, piece);
    }

    size_t synced = 0, silent = 0, bursts = totalBytes / burst;
    for (size_t b = 0; b < bursts; ++b) {
        const uint8_t* p = stream.data() + b * burst;
        if (p[0] == 0x72 && p[1] == 0xF8 && p[2] == 0x1F && p[3] == 0x4E) {
            ++synced;
        } else {
            bool zero = true;
            for (size_t i = 0; i < burst && zero; ++i) zero = p[i] == 0;
            if (zero) ++silent;
        }
    }
    CHECK(synced + silent == bursts, "%zu of %zu bursts neither synced nor silent",
          bursts - synced - silent, bursts);
    CHECK(synced >= bursts - 2, "only %zu of %zu bursts carry the sync words",
          synced, bursts);

    wchar_t tempDirectory[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDirectory);
    const std::string path = narrow(tempDirectory) + "ac3spdif-roundtrip.spdif";
    FILE* file = nullptr;
    fopen_s(&file, path.c_str(), "wb");
    CHECK(file != nullptr, "cannot write %s", path.c_str());
    if (!file) return;
    fwrite(stream.data(), 1, stream.size(), file);
    fclose(file);

    AVFormatContext* input = nullptr;
    const int opened =
        avformat_open_input(&input, path.c_str(), av_find_input_format("spdif"), nullptr);
    CHECK(opened >= 0, "spdif demuxer could not open the stream");
    if (opened < 0) return;

    AVCodecContext* decoder = nullptr;
    std::map<int, std::vector<float>> channels;
    AVChannelLayout decodedLayout{};
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    double rate = config.sampleRate;
    while (av_read_frame(input, packet) >= 0) {
        if (!decoder) {
            const AVCodecParameters* parameters = input->streams[packet->stream_index]->codecpar;
            const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
            CHECK(codec != nullptr, "no decoder for codec id %d", parameters->codec_id);
            if (!codec) break;
            decoder = avcodec_alloc_context3(codec);
            avcodec_open2(decoder, codec, nullptr);
        }
        if (avcodec_send_packet(decoder, packet) >= 0) {
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                if (decodedLayout.nb_channels == 0) {
                    av_channel_layout_copy(&decodedLayout, &frame->ch_layout);
                    rate = frame->sample_rate;
                }
                for (int c = 0; c < frame->ch_layout.nb_channels; ++c) {
                    for (int i = 0; i < frame->nb_samples; ++i) {
                        channels[c].push_back(sampleAt(frame, c, i));
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    CHECK(decodedLayout.nb_channels == 6, "decoded %d channels, expected 6",
          decodedLayout.nb_channels);
    static const double candidates[] = {440, 554, 659, 82, 880, 1046};
    for (int c = 0; c < decodedLayout.nb_channels; ++c) {
        const AVChannel position = av_channel_layout_channel_from_index(&decodedLayout, c);
        const double expected = expectedPitch(position);
        char name[16] = {};
        av_channel_name(name, sizeof(name), position);
        const std::vector<float>& samples = channels[c];
        const size_t start = static_cast<size_t>(0.5 * rate);
        const size_t count = static_cast<size_t>(1.5 * rate);
        double best = 0, bestPitch = 0, runnerUp = 0;
        for (const double pitch : candidates) {
            const double power = goertzelPower(samples, start, count, pitch, rate);
            if (power > best) {
                runnerUp = best;
                best = power;
                bestPitch = pitch;
            } else if (power > runnerUp) {
                runnerUp = power;
            }
        }
        const double margin = runnerUp > 0 ? best / runnerUp : 1e12;
        std::printf("  %-4s expected %5.0f Hz  detected %5.0f Hz  margin %.0fx\n",
                    name, expected, bestPitch, margin);
        CHECK(bestPitch == expected, "%s carries %.0f Hz, expected %.0f Hz", name,
              bestPitch, expected);
        CHECK(margin > 100, "%s margin only %.1fx", name, margin);
    }

    av_channel_layout_uninit(&decodedLayout);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    DeleteFileA(path.c_str());
}

// A capture clock and an output clock, in simulated milliseconds. The capture
// side delivers a packet every `packetMs` at `rate * (1 + ppm)`; the output
// side pulls one 10 ms device period at a time at exactly the wire rate.
struct Simulation {
    Config config;
    ByteRing ring;
    Encoder encoder;
    std::vector<float> packet;
    std::vector<uint8_t> period;
    double owedFrames = 0;
    double phase = 0;
    int64_t millisecond = 0;
    int stalledFor = 0;

    Simulation(const Config& c)
        : config(c),
          ring(static_cast<size_t>(6 * 4 * 48000)),
          encoder(c, 48000, 6, 0x3F, &ring),
          period(480 * 4) {
        encoder.open();
    }

    // `ppm` > 0 means the capture clock runs fast. `deliver` false stops
    // capture outright (nothing playing); `stallMs` holds packets back and then
    // delivers the backlog at once, which is what a descheduled capture
    // thread looks like from the ring's side.
    void run(double seconds, double ppm, bool deliver = true) {
        const int64_t end = millisecond + static_cast<int64_t>(seconds * 1000);
        for (; millisecond < end; ++millisecond) {
            if (millisecond % 10 == 3) {
                owedFrames += 480.0 * (1.0 + ppm * 1e-6);
                if (!deliver) {
                    owedFrames = 0;
                } else if (stalledFor > 0) {
                    stalledFor -= 10;
                } else {
                    const int frames = static_cast<int>(owedFrames);
                    owedFrames -= frames;
                    packet.resize(static_cast<size_t>(frames) * 6);
                    for (int f = 0; f < frames; ++f) {
                        const float value = static_cast<float>(0.1 * std::sin(phase));
                        phase += 2 * 3.14159265358979 * 440 / 48000;
                        for (int c = 0; c < 6; ++c) packet[f * 6 + c] = value;
                    }
                    ring.write(packet.data(), packet.size() * sizeof(float));
                }
            }
            if (millisecond % 10 == 0) {
                encoder.pull(period.data(), period.size());
            }
        }
    }
};

static void testDrift(OutputCodec which, double ppm) {
    Config config;
    config.codec = which;
    config.bufferMs = 30;
    Simulation simulation(config);
    simulation.run(60, ppm);  // converge
    const EncoderStats before = simulation.encoder.stats();
    simulation.run(120, ppm);
    const EncoderStats after = simulation.encoder.stats();

    const double marginMs = after.marginFrames * 1000.0 / 48000;
    const int64_t consumed = after.inputFramesConsumed - before.inputFramesConsumed;
    const double measuredPpm =
        static_cast<double>((after.samplesDropped - before.samplesDropped) -
                            (after.samplesDuplicated - before.samplesDuplicated)) *
        1e6 / static_cast<double>(consumed > 0 ? consumed : 1);
    std::printf("drift: %-20s capture %+6.0f ppm  corrected %+7.1f ppm  "
                "margin %.1f ms (target 30)  dropouts %lld\n",
                codec::name(which), ppm, measuredPpm, marginMs,
                static_cast<long long>(after.dropouts));

    CHECK(after.dropouts == 0, "%lld dropouts at %+.0f ppm",
          static_cast<long long>(after.dropouts), ppm);
    CHECK(after.silenceFrames == before.silenceFrames,
          "%lld silence frames after convergence",
          static_cast<long long>(after.silenceFrames - before.silenceFrames));
    // Tight enough to catch proportional-only control, which settles a
    // window's worth of drift — about 3 ms at 3000 ppm — off target.
    CHECK(std::fabs(marginMs - 30) <= 1.5, "margin %.1f ms, target 30", marginMs);
    CHECK(std::fabs(measuredPpm - ppm) <= std::fabs(ppm) * 0.1 + 20,
          "corrected %+.1f ppm for a %+.0f ppm clock error", measuredPpm, ppm);
}

static void testStarvation() {
    std::printf("starvation: idle versus dropout\n");
    Config config;
    config.bufferMs = 30;
    Simulation simulation(config);
    simulation.run(5, 0);
    CHECK(simulation.encoder.stats().dropouts == 0,
          "start-up fill counted as a dropout");

    simulation.run(2, 0, false);  // playback stops
    simulation.run(5, 0);         // and resumes
    const EncoderStats idle = simulation.encoder.stats();
    std::printf("  after a 2 s pause: %lld silence frames, %lld dropouts\n",
                static_cast<long long>(idle.silenceFrames),
                static_cast<long long>(idle.dropouts));
    CHECK(idle.dropouts == 0, "a pause in playback was counted as a dropout");
    CHECK(idle.silenceFrames > 0, "a pause produced no silence frames");

    simulation.stalledFor = 80;  // longer than the 30 ms margin
    simulation.run(5, 0);
    const EncoderStats stalled = simulation.encoder.stats();
    std::printf("  after an 80 ms capture stall: %lld dropouts\n",
                static_cast<long long>(stalled.dropouts));
    CHECK(stalled.dropouts == 1, "an 80 ms stall produced %lld dropouts, expected 1",
          static_cast<long long>(stalled.dropouts));

    simulation.stalledFor = 20;  // shorter than the margin: absorbed
    simulation.run(5, 0);
    CHECK(simulation.encoder.stats().dropouts == 1,
          "a 20 ms stall inside a 30 ms margin caused a dropout");
}

static void testTables() {
    std::printf("tables\n");
    CHECK(codec::bitsPerSecond("640k") == 640000, "640k");
    CHECK(codec::bitsPerSecond("1509k") == 1509000, "1509k");
    CHECK(codec::frameSamples(OutputCodec::dts) == 512, "DTS frame");
    CHECK(codec::burstBytes(OutputCodec::ac3) == 6144, "AC-3 burst");
    CHECK(Config().encodeLayout(8) == "5.1", "7.1 capture encodes as 5.1");
    CHECK(codec::encodeLayout(OutputCodec::dts, 6) == "5.1(side)", "DTS layout");
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        testTables();
        testRoundTrip(OutputCodec::ac3);
        testRoundTrip(OutputCodec::dts);
        for (const double ppm : {0.0, 300.0, -300.0, 3000.0, -3000.0}) {
            testDrift(OutputCodec::ac3, ppm);
        }
        testDrift(OutputCodec::dts, 300);
        testDrift(OutputCodec::dts, -3000);
        testStarvation();
    } catch (const Failure& failure) {
        std::printf("  FAIL  threw: %s\n", failure.what());
        ++g_failures;
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
