#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace baudline {

// Single-producer single-consumer lock-free ring buffer for audio data.
// Producer: audio callback thread.  Consumer: main/render thread.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(nextPow2(capacity))
        , mask_(capacity_ - 1)
        , buf_(capacity_)
    {}

    // Returns number of items available to read.
    size_t available() const {
        return writePos_.load(std::memory_order_acquire)
             - readPos_.load(std::memory_order_relaxed);
    }

    size_t freeSpace() const {
        return capacity_ - available();
    }

    // Write up to `count` items.  Returns number actually written.
    size_t write(const T* data, size_t count) {
        const size_t wp   = writePos_.load(std::memory_order_relaxed);
        const size_t rp   = readPos_.load(std::memory_order_acquire);
        const size_t free = capacity_ - (wp - rp);
        if (count > free) count = free;
        if (count == 0) return 0;

        const size_t idx  = wp & mask_;
        const size_t tail = capacity_ - idx;
        if (count <= tail) {
            std::memcpy(&buf_[idx], data, count * sizeof(T));
        } else {
            std::memcpy(&buf_[idx], data, tail * sizeof(T));
            std::memcpy(&buf_[0], data + tail, (count - tail) * sizeof(T));
        }
        writePos_.store(wp + count, std::memory_order_release);
        return count;
    }

    // Read up to `count` items.  Returns number actually read.
    size_t read(T* data, size_t count) {
        const size_t rp   = readPos_.load(std::memory_order_relaxed);
        const size_t wp   = writePos_.load(std::memory_order_acquire);
        const size_t avail = wp - rp;
        if (count > avail) count = avail;
        if (count == 0) return 0;

        const size_t idx  = rp & mask_;
        const size_t tail = capacity_ - idx;
        if (count <= tail) {
            std::memcpy(data, &buf_[idx], count * sizeof(T));
        } else {
            std::memcpy(data, &buf_[idx], tail * sizeof(T));
            std::memcpy(data + tail, &buf_[0], (count - tail) * sizeof(T));
        }
        readPos_.store(rp + count, std::memory_order_release);
        return count;
    }

    // Discard up to `count` items without copying.
    size_t discard(size_t count) {
        const size_t rp   = readPos_.load(std::memory_order_relaxed);
        const size_t wp   = writePos_.load(std::memory_order_acquire);
        const size_t avail = wp - rp;
        if (count > avail) count = avail;
        readPos_.store(rp + count, std::memory_order_release);
        return count;
    }

    void reset() {
        readPos_.store(0, std::memory_order_relaxed);
        writePos_.store(0, std::memory_order_relaxed);
    }

private:
    static size_t nextPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buf_;
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
};

} // namespace baudline
