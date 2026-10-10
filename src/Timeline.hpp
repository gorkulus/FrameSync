#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace framesync {
constexpr int CHANNELS = 16;

// Absolute rational boundaries: no accumulated floating-point timer error.
inline int64_t frameSample(int64_t frame, int rate, int fps = 60) {
    int64_t numerator = frame * int64_t(rate);
    return numerator >= 0 ? (numerator + fps - 1) / fps : numerator / fps;
}

// Fixed storage, used only by the engine thread. Supports four frames of
// history through 768 kHz without allocating in process().
struct CvHistory {
    static constexpr size_t CAPACITY = 65536;
    std::array<std::array<float, CHANNELS>, CAPACITY> values{};
    int64_t oldest = 0, newest = -1;
    int rate = 0;
    void write(int64_t sample, int sampleRate, const std::array<float, CHANNELS>& cv) {
        if (sample != newest + 1 || rate != sampleRate) oldest = sample;
        rate = sampleRate; newest = sample;
        if (newest - oldest >= int64_t(CAPACITY)) oldest = newest - CAPACITY + 1;
        values[uint64_t(sample) % CAPACITY] = cv;
    }
    std::array<float, CHANNELS> read(int64_t requested, int64_t& actual) const {
        actual = requested < oldest ? oldest : (requested > newest ? newest : requested);
        return values[uint64_t(actual) % CAPACITY];
    }
};

constexpr int AUDIO_BLOCK_SAMPLES = 1024;
struct AudioBlock {
    uint64_t generation = 0;
    int64_t firstSample = 0;
    int count = 0;
    std::array<float, AUDIO_BLOCK_SAMPLES * 2> samples{};
};

template <typename T, size_t N>
class SpscQueue {
    std::array<T, N> data_{};
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
public:
    T* reserve() {
        size_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= N) return nullptr;
        return &data_[w % N];
    }
    void commit() { write_.fetch_add(1, std::memory_order_release); }
    T* front() {
        size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return nullptr;
        return &data_[r % N];
    }
    void pop() { read_.fetch_add(1, std::memory_order_release); }
    bool empty() const { return read_.load() == write_.load(); }
    // Only while the producer and consumer are quiescent.
    void reset() { read_ = 0; write_ = 0; }
    template <typename F> void initialize(F f) { for (auto& v : data_) f(v); }
};

struct Event {
    enum Type { Start, Frame, Stop } type = Frame;
    int64_t index = 0;
    int64_t sample = 0;
    int64_t cvSample = 0;
    bool cvClamped = false;
    uint64_t generation = 0;
    std::array<float, CHANNELS> cv{};
};
}
