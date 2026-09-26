// Single-producer / single-consumer lock-free byte ring.
//
// One end of every ring in this program is an audio thread running under MMCSS
// "Pro Audio" scheduling, where blocking on a mutex risks a dropout. Absolute
// monotonic 64-bit indices keep the arithmetic trivial: an int64_t would need
// ~600 years at 48 kHz stereo to overflow, so wraparound is not a case worth
// handling.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace ac3spdif {

class ByteRing {
public:
    explicit ByteRing(size_t capacity)
        : capacity_(capacity), storage_(new uint8_t[capacity]()) {}

    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;

    size_t capacity() const { return capacity_; }

    // Bytes currently buffered. Safe to call from either side; the value is a
    // snapshot and may be stale by the time it is used, which is fine for the
    // drift controller that consumes it.
    size_t fillLevel() const {
        return static_cast<size_t>(
            writeIndex_.load(std::memory_order_acquire) -
            readIndex_.load(std::memory_order_acquire));
    }

    size_t freeSpace() const { return capacity_ - fillLevel(); }

    // Producer side. Returns the number of bytes actually written, which is
    // less than `count` when the ring is full.
    size_t write(const void* source, size_t count) {
        const int64_t write = writeIndex_.load(std::memory_order_relaxed);
        const int64_t read = readIndex_.load(std::memory_order_acquire);
        const size_t writable =
            count < capacity_ - static_cast<size_t>(write - read)
                ? count : capacity_ - static_cast<size_t>(write - read);
        if (writable == 0) return 0;

        const size_t offset = static_cast<size_t>(write % capacity_);
        const size_t firstChunk =
            writable < capacity_ - offset ? writable : capacity_ - offset;
        std::memcpy(storage_.get() + offset, source, firstChunk);
        if (writable > firstChunk) {
            std::memcpy(storage_.get(),
                        static_cast<const uint8_t*>(source) + firstChunk,
                        writable - firstChunk);
        }
        writeIndex_.store(write + static_cast<int64_t>(writable),
                          std::memory_order_release);
        return writable;
    }

    // Consumer side. Returns the number of bytes actually read.
    size_t read(void* destination, size_t count) {
        const int64_t read = readIndex_.load(std::memory_order_relaxed);
        const int64_t write = writeIndex_.load(std::memory_order_acquire);
        const size_t readable =
            count < static_cast<size_t>(write - read)
                ? count : static_cast<size_t>(write - read);
        if (readable == 0) return 0;

        const size_t offset = static_cast<size_t>(read % capacity_);
        const size_t firstChunk =
            readable < capacity_ - offset ? readable : capacity_ - offset;
        std::memcpy(destination, storage_.get() + offset, firstChunk);
        if (readable > firstChunk) {
            std::memcpy(static_cast<uint8_t*>(destination) + firstChunk,
                        storage_.get(), readable - firstChunk);
        }
        readIndex_.store(read + static_cast<int64_t>(readable),
                         std::memory_order_release);
        return readable;
    }

    // Consumer side, for realtime use: always fills `count` bytes, zero-padding
    // whatever the ring could not supply. Returns the shortfall so the caller
    // can count underruns.
    size_t readZeroPadded(void* destination, size_t count) {
        const size_t got = read(destination, count);
        if (got < count) {
            std::memset(static_cast<uint8_t*>(destination) + got, 0, count - got);
        }
        return count - got;
    }

    // Discard buffered bytes without copying them out.
    void discard(size_t count) {
        const int64_t read = readIndex_.load(std::memory_order_relaxed);
        const int64_t write = writeIndex_.load(std::memory_order_acquire);
        const size_t droppable =
            count < static_cast<size_t>(write - read)
                ? count : static_cast<size_t>(write - read);
        if (droppable == 0) return;
        readIndex_.store(read + static_cast<int64_t>(droppable),
                         std::memory_order_release);
    }

private:
    const size_t capacity_;
    std::unique_ptr<uint8_t[]> storage_;
    std::atomic<int64_t> writeIndex_{0};
    std::atomic<int64_t> readIndex_{0};
};

}  // namespace ac3spdif
