#pragma once
#include "Timeline.hpp"
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <memory>

namespace framesync {
enum class State { Idle, Preparing, Priming, Ready, Recording, Finishing, Error };

struct Config {
    std::string directory;
    std::string ffmpegPath; // UTF-8; empty uses FRAMESYNC_FFMPEG or platform default/PATH.
    std::string prefix = "take";
    int size = 512;
    int fps = 60;
    int sampleRate = 48000;
    int x = 0, y = 0; // top-left framebuffer pixels, relative to Rack's window
    int crf = 18;
    int cvDelayFrames = 0; // Delay CSV values relative to the unchanged audio/video.
    std::array<std::string, CHANNELS> names;
    Config();
};

struct Pixels {
    std::vector<uint8_t> rgba;
    int64_t sample = 0;
    uint64_t serial = 0;
    uint64_t generation = 0;
};

class CaptureSession {
public:
    CaptureSession();
    ~CaptureSession();
    bool arm(const Config& config); // GUI thread, never the audio callback
    void disarm();
    bool begin(int64_t engineSample); // audio thread
    bool frame(int64_t index, int64_t engineSample, const std::array<float, CHANNELS>& cv,
               int64_t cvSample = INT64_MIN, bool cvClamped = false);
    bool audio(int64_t engineSample, float left, float right); // normalized +/-1, engine thread
    void end(int64_t engineSample);
    bool submitPixels(const uint8_t* topDownRgba, int64_t engineSample); // GUI thread
    void fail(const std::string& reason); // GUI thread
    State state() const { return state_.load(std::memory_order_acquire); }
    bool wantsPixels() const;
    std::string message() const;
    std::string lastTake() const;
    uint64_t frameCount() const { return encoded_.load(); }
    uint64_t repeats() const { return repeated_.load(); }
    uint64_t completed() const { return completed_.load(); }
    uint64_t missedImages() const { return missedImages_.load(); }
    void shutdown();

private:
    void run();
    void setMessage(const std::string& text);
    bool push(const Event& e);
    bool flushAudio();
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> quit_{false}, keepArmed_{false}, overflow_{false}, failed_{false};
    std::atomic<bool> audioOverflow_{false};
    std::atomic<uint64_t> encoded_{0}, repeated_{0}, completed_{0}, missedImages_{0};
    std::atomic<uint64_t> generation_{0};
    std::atomic<size_t> pixelBytes_{0};
    std::mutex mutex_;
    mutable std::mutex textMutex_;
    std::condition_variable wake_;
    Config config_;
    std::string message_ = "Choose a folder and region, then arm";
    std::string lastTake_;
    SpscQueue<Event, 4096> events_;
    std::unique_ptr<SpscQueue<AudioBlock, 256>> audioBlocks_{new SpscQueue<AudioBlock, 256>};
    AudioBlock audioPartial_; // engine producer only
    SpscQueue<Pixels, 16> pixels_;
    uint64_t imageSerial_ = 0; // GUI producer only
    std::thread worker_;
};
}
