#include "Encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <mutex>

namespace ac3spdif {

// FFmpeg logs through one process-wide callback, so the encoder that is
// currently open registers itself as the destination. Only errors are let
// through; on the success path FFmpeg says nothing.
static std::mutex g_logLock;
static Encoder* g_logTarget = nullptr;
static std::string g_lastError;

static void avLogCallback(void* context, int level, const char* format,
                          va_list arguments) {
    if (level > av_log_get_level()) return;
    char line[512];
    int printPrefix = 1;
    av_log_format_line2(context, level, format, arguments, line, sizeof(line),
                        &printPrefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) return;
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> guard(g_logLock);
        g_lastError = text;
        if (g_logTarget) sink = g_logTarget->onLog;
    }
    if (sink) sink("ffmpeg: " + text);
}

static std::string avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

static void check(int code, const std::string& what) {
    if (code < 0) throw Failure(what + ": " + avError(code));
}

// The self-test source layout, as a speaker layout rather than a codec one: an
// 8-channel test is a 7.1 source the encoder folds, exactly like a capture.
static const char* sourceLayoutName(int channels) {
    switch (channels) {
        case 1: return "mono";
        case 2: return "stereo";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return nullptr;
    }
}

static int popcount(uint64_t mask) {
    int count = 0;
    for (; mask != 0; mask &= mask - 1) ++count;
    return count;
}

Encoder::Encoder(const Config& config, double inputRate, int inputChannels,
                 uint64_t inputMask, ByteRing* pcmRing)
    : config_(config),
      inputRate_(inputRate),
      inputChannels_(inputChannels),
      inputMask_(inputMask),
      pcmRing_(pcmRing) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    ticksPerSecond_ = frequency.QuadPart;
}

Encoder::~Encoder() {
    {
        std::lock_guard<std::mutex> guard(g_logLock);
        if (g_logTarget == this) g_logTarget = nullptr;
    }
    dumpRunning_.store(false, std::memory_order_release);
    if (dumpThread_.joinable()) dumpThread_.join();

    if (muxer_) {
        if (muxer_->pb) {
            av_freep(&muxer_->pb->buffer);
            avio_context_free(&muxer_->pb);
        }
        avformat_free_context(muxer_);
    }
    if (resampled_) {
        av_freep(&resampled_[0]);
        av_freep(&resampled_);
    }
    swr_free(&resampler_);
    if (fifo_) av_audio_fifo_free(fifo_);
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
}

void Encoder::open() {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(avLogCallback);
    {
        std::lock_guard<std::mutex> guard(g_logLock);
        g_logTarget = this;
    }

    const char* encoderName = codec::ffmpegEncoder(config_.codec);
    const AVCodec* encoder = avcodec_find_encoder_by_name(encoderName);
    if (!encoder) {
        throw Failure(std::string("the bundled FFmpeg has no ") + encoderName +
                      " encoder — rebuild it with scripts\\build-ffmpeg.ps1");
    }
    codec_ = avcodec_alloc_context3(encoder);
    if (!codec_) throw Failure("cannot allocate the encoder context");

    const int wireRate = static_cast<int>(config_.sampleRate);
    codec_->bit_rate = codec::bitsPerSecond(config_.effectiveBitrate());
    codec_->sample_rate = wireRate;
    codec_->time_base = AVRational{1, wireRate};
    // One thread: frame threading would add whole frames of delay to buy
    // throughput this encoder does not need.
    codec_->thread_count = 1;
    if (codec::needsExperimentalFlag(config_.codec)) {
        codec_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }
    const std::string encodeLayout = config_.encodeLayout(inputChannels_);
    check(av_channel_layout_from_string(&codec_->ch_layout, encodeLayout.c_str()),
          "unknown encode layout " + encodeLayout);

    // The AC-3 encoder takes planar float and the DTS encoder interleaved
    // 32-bit integer; ask rather than hard-code either.
    const void* formats = nullptr;
    int formatCount = 0;
    check(avcodec_get_supported_config(codec_, encoder,
                                       AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                       &formats, &formatCount),
          "cannot query the encoder's sample formats");
    codec_->sample_fmt = formats && formatCount > 0
                             ? static_cast<const AVSampleFormat*>(formats)[0]
                             : AV_SAMPLE_FMT_FLTP;

    check(avcodec_open2(codec_, encoder, nullptr),
          std::string("cannot open the ") + codec::name(config_.codec) +
              " encoder at " + config_.effectiveBitrate() + ", " + encodeLayout);
    frameSamples_ = codec_->frame_size;
    if (frameSamples_ != config_.frameSamples()) {
        throw Failure("the " + std::string(encoderName) + " encoder reports " +
                      std::to_string(frameSamples_) + "-sample frames; this "
                      "build expects " + std::to_string(config_.frameSamples()));
    }
    encoderDelayFrames_ = codec_->initial_padding;

    // What the capture actually is. The full speaker mask when it describes
    // the stream, otherwise the conventional layout for the channel count.
    AVChannelLayout inputLayout{};
    if (!pcmRing_) {
        const char* name = sourceLayoutName(inputChannels_);
        if (name) check(av_channel_layout_from_string(&inputLayout, name), name);
        else av_channel_layout_default(&inputLayout, inputChannels_);
    } else if (!config_.explicitLayout.empty()) {
        check(av_channel_layout_from_string(&inputLayout,
                                            config_.explicitLayout.c_str()),
              "unknown layout " + config_.explicitLayout);
        if (inputLayout.nb_channels != inputChannels_) {
            av_channel_layout_uninit(&inputLayout);
            throw Failure("--layout " + config_.explicitLayout + " has " +
                          "the wrong number of channels for a " +
                          std::to_string(inputChannels_) + "-channel capture");
        }
    } else if (inputMask_ != 0 && popcount(inputMask_) == inputChannels_) {
        check(av_channel_layout_from_mask(&inputLayout, inputMask_),
              "unusable channel mask");
    } else {
        av_channel_layout_default(&inputLayout, inputChannels_);
    }

    // Interleaved float in, whatever the encoder wants out: packing, sample
    // format, the downmix to the encode layout (7.1 folds its side pair into
    // the back pair here), and the mix rate to the wire rate.
    SwrContext* resampler = nullptr;
    const int configured = swr_alloc_set_opts2(
        &resampler, &codec_->ch_layout, codec_->sample_fmt, wireRate,
        &inputLayout, AV_SAMPLE_FMT_FLT, static_cast<int>(inputRate_), 0,
        nullptr);
    av_channel_layout_uninit(&inputLayout);
    check(configured, "cannot configure the resampler");
    resampler_ = resampler;
    check(swr_init(resampler_), "cannot initialise the resampler");

    nominalInputFrames_ = static_cast<int>(
        std::ceil(frameSamples_ * inputRate_ / config_.sampleRate));
    targetMarginFrames_ =
        static_cast<int>(config_.bufferMs * inputRate_ / 1000.0 + 0.5);
    // maxDriftFrames is per 1536 wire samples; scale it so the ppm ceiling
    // is the same for every frame length and every capture rate.
    maxCorrectionFrames_ = static_cast<int>(
        config_.maxDriftFrames * (static_cast<double>(frameSamples_) / 1536.0) *
            (inputRate_ / config_.sampleRate) + 0.5);
    if (maxCorrectionFrames_ < 1) maxCorrectionFrames_ = 1;
    // A quarter of a millisecond: the integral term does the steady work, so
    // the proportional one only needs to ignore sub-jitter noise.
    deadbandFrames_ = static_cast<int>(inputRate_ / 4000.0);
    if (deadbandFrames_ < 2) deadbandFrames_ = 2;
    // Anything this far above target is a backlog — a render stall, a device
    // renegotiating — and is dropped at once rather than trimmed a sample at
    // a time for minutes.
    backlogFrames_ = static_cast<int>(0.1 * inputRate_);
    if (backlogFrames_ < 3 * targetMarginFrames_) {
        backlogFrames_ = 3 * targetMarginFrames_;
    }
    windowFrames_ = static_cast<int>(config_.sampleRate / frameSamples_ + 0.5);
    if (windowFrames_ < 4) windowFrames_ = 4;
    // A starvation episode that ends within a quarter second was a gap in
    // audio that was otherwise playing; one that lasts longer was playback
    // stopping. Loopback delivers no packets at all when nothing plays, so
    // the two look identical until the episode ends.
    dropoutLimitFrames_ = static_cast<int>(
        std::ceil(0.25 * config_.sampleRate / frameSamples_));

    const int inputCapacity = nominalInputFrames_ + maxCorrectionFrames_ + 64;
    input_.assign(static_cast<size_t>(inputCapacity) * inputChannels_, 0.0f);
    resampledCapacity_ = static_cast<int>(
        std::ceil(inputCapacity * config_.sampleRate / inputRate_)) + 256;
    check(av_samples_alloc_array_and_samples(
              &resampled_, nullptr, codec_->ch_layout.nb_channels,
              resampledCapacity_, codec_->sample_fmt, 0),
          "cannot allocate the resampler output");
    fifo_ = av_audio_fifo_alloc(codec_->sample_fmt,
                                codec_->ch_layout.nb_channels,
                                frameSamples_ * 4);
    if (!fifo_) throw Failure("cannot allocate the sample FIFO");

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) throw Failure("cannot allocate encoder buffers");
    frame_->format = codec_->sample_fmt;
    check(av_channel_layout_copy(&frame_->ch_layout, &codec_->ch_layout),
          "cannot copy the channel layout");
    frame_->sample_rate = wireRate;
    frame_->nb_samples = frameSamples_;
    check(av_frame_get_buffer(frame_, 0), "cannot allocate the encoder frame");

    // The IEC 61937 muxer, writing through a custom I/O context straight into
    // the carry buffer: one burst per packet, preamble and padding included,
    // byte for byte what `ffmpeg -f spdif` emits.
    check(avformat_alloc_output_context2(&muxer_, nullptr, "spdif", nullptr),
          "the bundled FFmpeg has no spdif muxer");
    AVStream* stream = avformat_new_stream(muxer_, nullptr);
    if (!stream) throw Failure("cannot create the spdif stream");
    check(avcodec_parameters_from_context(stream->codecpar, codec_),
          "cannot describe the encoded stream to the muxer");
    stream->time_base = codec_->time_base;
    const int ioSize = config_.burstBytes() * 2;
    auto* ioBuffer = static_cast<unsigned char*>(av_malloc(ioSize));
    if (!ioBuffer) throw Failure("cannot allocate the muxer buffer");
    muxer_->pb = avio_alloc_context(ioBuffer, ioSize, 1, this, nullptr,
                                    &Encoder::writePacketThunk, nullptr);
    if (!muxer_->pb) {
        av_free(ioBuffer);
        throw Failure("cannot allocate the muxer I/O context");
    }
    muxer_->flags |= AVFMT_FLAG_CUSTOM_IO;
    check(avformat_write_header(muxer_, nullptr),
          "the spdif muxer refused the encoded stream");

    // Room for the unsent part of one burst plus the next, plus the largest
    // pull any sink makes; pull() splits anything bigger.
    carry_.assign(static_cast<size_t>(config_.burstBytes()) * 2 + 65536, 0);

    if (!pcmRing_) {
        // One steady tone per channel, each a different pitch, so a
        // listener can confirm not just that decoding engaged but that the
        // channels arrived in the right order.
        static const double pitches[] = {440, 554, 659, 82, 880, 1046, 1318, 1568};
        phase_.assign(inputChannels_, 0.0);
        phaseStep_.resize(inputChannels_);
        for (int channel = 0; channel < inputChannels_; ++channel) {
            phaseStep_[channel] =
                2.0 * 3.14159265358979323846 * pitches[channel % 8] / inputRate_;
        }
        starving_ = false;
    }

    if (!config_.dumpPath.empty()) {
        dumpFile_.reset(CreateFileW(widen(config_.dumpPath).c_str(),
                                    GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
        if (!dumpFile_) {
            throw Failure("cannot open dump file " + config_.dumpPath + ": " +
                          hrDescription(HRESULT_FROM_WIN32(GetLastError())));
        }
        dumpRing_ = std::make_unique<ByteRing>(
            static_cast<size_t>(config_.burstBytes()) * 512);
        dumpRunning_.store(true, std::memory_order_release);
        dumpThread_ = std::thread([this] { dumpLoop(); });
    }
}

// MARK: - Realtime path

void Encoder::pull(uint8_t* destination, size_t bytes) {
    const size_t burst = static_cast<size_t>(config_.burstBytes());
    const size_t maxPiece = carry_.size() - 2 * burst;
    while (bytes > 0) {
        const size_t piece = bytes < maxPiece ? bytes : maxPiece;
        while (carryLength_ < piece) {
            if (!encodeNextBurst()) {
                // An encode failure still has to fill the wire, and a zeroed
                // burst is the one filler a receiver treats as a pause.
                encodeErrors_.fetch_add(1, std::memory_order_relaxed);
                appendSilenceToCarry(burst);
            }
        }
        std::memcpy(destination, carry_.data() + carryStart_, piece);
        carryStart_ += piece;
        carryLength_ -= piece;
        destination += piece;
        bytes -= piece;
    }
}

bool Encoder::encodeNextBurst() {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    if (!produceFrame()) return false;
    frame_->pts = nextPts_;
    nextPts_ += frameSamples_;
    if (avcodec_send_frame(codec_, frame_) < 0) return false;

    bool produced = false;
    for (;;) {
        const int received = avcodec_receive_packet(codec_, packet_);
        if (received == AVERROR(EAGAIN)) break;
        if (received < 0) return false;
        av_packet_rescale_ts(packet_, codec_->time_base,
                             muxer_->streams[0]->time_base);
        packet_->stream_index = 0;
        const size_t before = carryLength_;
        const int muxed = av_write_frame(muxer_, packet_);
        av_packet_unref(packet_);
        if (muxed < 0) return false;
        avio_flush(muxer_->pb);
        const size_t written = carryLength_ - before;
        burstBytes_.fetch_add(static_cast<int64_t>(written),
                              std::memory_order_relaxed);
        if (dumpRing_) writeDump(carry_.data() + carryStart_ + before, written);
        produced = true;
    }
    // An encoder still priming emits nothing for its first frame or two;
    // the wire still needs a burst's worth of bytes in that slot.
    if (!produced) appendSilenceToCarry(static_cast<size_t>(config_.burstBytes()));

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    const int64_t ticks = end.QuadPart - start.QuadPart;
    encodeTicksTotal_.fetch_add(ticks, std::memory_order_relaxed);
    if (ticks > encodeTicksMax_.load(std::memory_order_relaxed)) {
        encodeTicksMax_.store(ticks, std::memory_order_relaxed);
    }
    framesEncoded_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Fill `frame_` with one frame of encoder-format samples: from capture, from
// the tone generator, or — when capture has nothing — silence, so the
// receiver keeps its lock through a pause instead of re-syncing afterwards.
bool Encoder::produceFrame() {
    if (av_frame_make_writable(frame_) < 0) return false;
    bool firstChunk = true;
    while (av_audio_fifo_size(fifo_) < frameSamples_) {
        // Demand-driven: exactly the input this frame is short of, so the
        // FIFO never accumulates resampled audio as hidden latency.
        const int shortfall = frameSamples_ - av_audio_fifo_size(fifo_);
        int nominal = static_cast<int>(
            std::ceil(shortfall * inputRate_ / config_.sampleRate));
        if (nominal < 1) nominal = 1;
        if (!pcmRing_) {
            synthesise(nominal);
            continue;
        }
        if (!takeInput(nominal, firstChunk)) {
            silenceFrames_.fetch_add(1, std::memory_order_relaxed);
            ++episodeFrames_;
            av_samples_set_silence(frame_->data, 0, frameSamples_,
                                   codec_->ch_layout.nb_channels,
                                   codec_->sample_fmt);
            return true;
        }
        firstChunk = false;
    }
    return av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame_->data),
                              frameSamples_) == frameSamples_;
}

// Take `nominal` frames' worth of capture — one more or fewer as the drift
// controller directs — and feed them to the resampler. Returns false when the
// capture ring cannot supply them, which puts the encoder into starvation
// until the full margin has been rebuilt.
bool Encoder::takeInput(int nominal, bool measure) {
    const size_t frameBytes = static_cast<size_t>(inputChannels_) * sizeof(float);
    int64_t available = static_cast<int64_t>(pcmRing_->fillLevel() / frameBytes);

    if (starving_) {
        // Resume only with the whole margin in hand, not merely one frame:
        // resuming at zero margin would starve again on the next packet.
        const int64_t resumeLevel = nominalInputFrames_ + targetMarginFrames_;
        if (available < resumeLevel) return false;
        // Trim to exactly the target. This is PCM, so any number of frames
        // can go — the old burst ring could only be trimmed a burst at a time.
        const int64_t excess = available - resumeLevel;
        if (excess > 0) {
            pcmRing_->discard(static_cast<size_t>(excess) * frameBytes);
            available -= excess;
        }
        if (episodeFromPlayback_ && episodeFrames_ <= dropoutLimitFrames_) {
            dropouts_.fetch_add(1, std::memory_order_relaxed);
        }
        starving_ = false;
        episodeFrames_ = 0;
        // The queued correction referred to a margin that no longer exists;
        // the learned clock error still holds, so it is kept.
        pendingCorrection_ = 0;
        feedforward_ = 0;
        windowMinimum_ = rawMinimum_ = INT64_MAX;
        windowCount_ = 0;
    }

    int dropped = 0;
    int duplicated = 0;
    if (measure) {
        // The margin this frame leaves behind, raw and as it will stand once
        // the proportional correction still queued has been applied.
        // Steering on the settled value, not the raw one, is what stops the
        // controller correcting the same error twice while the first
        // correction is still being spread across the window.
        const int64_t margin = available - nominal;
        if (margin < rawMinimum_) rawMinimum_ = margin;
        const int64_t settled = margin - pendingCorrection_;
        if (settled < windowMinimum_) windowMinimum_ = settled;
        ++windowCount_;

        // This frame's share: the queued proportional correction divided
        // evenly over what is left of the window, plus the feed-forward of
        // the learned clock error.
        int framesLeft = windowFrames_ - windowCount_ + 1;
        if (framesLeft < 1) framesLeft = 1;
        const int64_t proportional = static_cast<int64_t>(
            std::llround(static_cast<double>(pendingCorrection_) / framesLeft));
        feedforward_ += driftPerWindow_ / windowFrames_;
        const int64_t forward = static_cast<int64_t>(feedforward_);
        feedforward_ -= static_cast<double>(forward);
        const int64_t wanted = proportional + forward;
        const int64_t padLimit = maxCorrectionFrames_ < nominal - 1
                                     ? maxCorrectionFrames_ : nominal - 1;
        int64_t applied = wanted;
        if (applied > maxCorrectionFrames_) applied = maxCorrectionFrames_;
        if (applied < -padLimit) applied = -padLimit;
        // Take this frame's share out of the queue, and put back whatever the
        // per-frame ceiling held back.
        pendingCorrection_ += (wanted - applied) - proportional;
        if (applied > 0) dropped = static_cast<int>(applied);    // capture fast: shed
        else duplicated = static_cast<int>(-applied);           // capture slow: pad

        if (windowCount_ >= windowFrames_) {
            endWindow();
            available = static_cast<int64_t>(pcmRing_->fillLevel() / frameBytes);
        }
    }

    const int consume = nominal + dropped - duplicated;
    if (available < consume) {
        starving_ = true;
        episodeFromPlayback_ = true;
        episodeFrames_ = 0;
        return false;
    }

    pcmRing_->read(input_.data(), static_cast<size_t>(consume) * frameBytes);
    if (duplicated > 0) {
        // Repeat the final frame to bring the chunk back up to length.
        const float* last =
            input_.data() + static_cast<size_t>(consume - 1) * inputChannels_;
        for (int extra = 0; extra < duplicated; ++extra) {
            std::memcpy(input_.data() +
                            static_cast<size_t>(consume + extra) * inputChannels_,
                        last, frameBytes);
        }
    }
    // When shedding, the surplus tail frames are simply not passed on.
    feedResampler(input_.data(), nominal);

    inputFramesConsumed_.fetch_add(consume, std::memory_order_relaxed);
    if (dropped) samplesDropped_.fetch_add(dropped, std::memory_order_relaxed);
    if (duplicated) {
        samplesDuplicated_.fetch_add(duplicated, std::memory_order_relaxed);
    }
    return true;
}

// Close a measurement window: queue the correction that brings the settled
// trough back to the target, learn the clock error from it, or drop a
// backlog outright.
//
// The integral gain of 1/4 puts both roots of the loop at 1/2 — critically
// damped, so the learned rate converges within a few windows without ringing.
void Encoder::endWindow() {
    const int64_t error = windowMinimum_ - targetMarginFrames_;
    marginFrames_.store(static_cast<int>(rawMinimum_), std::memory_order_relaxed);
    windowMinimum_ = rawMinimum_ = INT64_MAX;
    windowCount_ = 0;

    if (error > backlogFrames_) {
        // A trim, not drift: not counted toward the correction rate, and
        // not learned from.
        const size_t frameBytes =
            static_cast<size_t>(inputChannels_) * sizeof(float);
        pcmRing_->discard(static_cast<size_t>(error) * frameBytes);
        return;
    }
    driftPerWindow_ += 0.25 * static_cast<double>(error);
    const double ceiling = static_cast<double>(maxCorrectionFrames_) * windowFrames_;
    if (driftPerWindow_ > ceiling) driftPerWindow_ = ceiling;
    if (driftPerWindow_ < -ceiling) driftPerWindow_ = -ceiling;
    if (error > deadbandFrames_ || error < -deadbandFrames_) {
        pendingCorrection_ += error;
    }
}

void Encoder::synthesise(int frames) {
    const int capacity = static_cast<int>(input_.size() / inputChannels_);
    if (frames > capacity) frames = capacity;
    for (int frame = 0; frame < frames; ++frame) {
        float* out = input_.data() + static_cast<size_t>(frame) * inputChannels_;
        for (int channel = 0; channel < inputChannels_; ++channel) {
            out[channel] = static_cast<float>(0.15 * std::sin(phase_[channel]));
            phase_[channel] += phaseStep_[channel];
            if (phase_[channel] > 6.283185307179586) {
                phase_[channel] -= 6.283185307179586;
            }
        }
    }
    feedResampler(input_.data(), frames);
}

void Encoder::feedResampler(const float* interleaved, int frames) {
    const uint8_t* planes[1] = {reinterpret_cast<const uint8_t*>(interleaved)};
    const int produced =
        swr_convert(resampler_, resampled_, resampledCapacity_, planes, frames);
    if (produced > 0) {
        av_audio_fifo_write(fifo_, reinterpret_cast<void**>(resampled_), produced);
    }
}

void Encoder::appendToCarry(const uint8_t* data, size_t bytes) {
    if (carryStart_ + carryLength_ + bytes > carry_.size()) {
        std::memmove(carry_.data(), carry_.data() + carryStart_, carryLength_);
        carryStart_ = 0;
    }
    const size_t room = carry_.size() - carryLength_;
    const size_t copied = bytes < room ? bytes : room;
    std::memcpy(carry_.data() + carryStart_ + carryLength_, data, copied);
    carryLength_ += copied;
}

void Encoder::appendSilenceToCarry(size_t bytes) {
    if (carryStart_ + carryLength_ + bytes > carry_.size()) {
        std::memmove(carry_.data(), carry_.data() + carryStart_, carryLength_);
        carryStart_ = 0;
    }
    const size_t room = carry_.size() - carryLength_;
    const size_t zeroed = bytes < room ? bytes : room;
    std::memset(carry_.data() + carryStart_ + carryLength_, 0, zeroed);
    carryLength_ += zeroed;
}

int Encoder::writePacketThunk(void* opaque, const uint8_t* data, int size) {
    static_cast<Encoder*>(opaque)->appendToCarry(data, static_cast<size_t>(size));
    return size;
}

void Encoder::writeDump(const uint8_t* data, size_t bytes) {
    // Non-blocking: a dump that cannot keep up loses bursts, never the wire.
    dumpRing_->write(data, bytes);
}

void Encoder::dumpLoop() {
    std::vector<uint8_t> chunk(65536);
    for (;;) {
        const bool running = dumpRunning_.load(std::memory_order_acquire);
        size_t got;
        while ((got = dumpRing_->read(chunk.data(), chunk.size())) > 0) {
            DWORD written = 0;
            WriteFile(dumpFile_.get(), chunk.data(), static_cast<DWORD>(got),
                      &written, nullptr);
        }
        if (!running) break;
        Sleep(20);
    }
}

// MARK: - Reporting

EncoderStats Encoder::stats() const {
    EncoderStats s;
    s.framesEncoded = framesEncoded_.load(std::memory_order_relaxed);
    s.inputFramesConsumed = inputFramesConsumed_.load(std::memory_order_relaxed);
    s.samplesDropped = samplesDropped_.load(std::memory_order_relaxed);
    s.samplesDuplicated = samplesDuplicated_.load(std::memory_order_relaxed);
    s.silenceFrames = silenceFrames_.load(std::memory_order_relaxed);
    s.dropouts = dropouts_.load(std::memory_order_relaxed);
    s.encodeErrors = encodeErrors_.load(std::memory_order_relaxed);
    s.burstBytes = burstBytes_.load(std::memory_order_relaxed);
    s.marginFrames = marginFrames_.load(std::memory_order_relaxed);
    const double perMs = static_cast<double>(ticksPerSecond_) / 1000.0;
    if (s.framesEncoded > 0) {
        s.encodeMsAverage =
            encodeTicksTotal_.load(std::memory_order_relaxed) / perMs /
            static_cast<double>(s.framesEncoded);
    }
    s.encodeMsMax = encodeTicksMax_.load(std::memory_order_relaxed) / perMs;
    return s;
}

std::string Encoder::lastError() const {
    std::lock_guard<std::mutex> guard(g_logLock);
    return g_lastError;
}

}  // namespace ac3spdif
