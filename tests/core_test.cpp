#include "CaptureSession.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cmath>
#include <thread>
#include <unistd.h>

using namespace framesync;
namespace fs = std::filesystem;
static void waitFor(CaptureSession& s, State expected) {
    for (int i = 0; i < 10000; ++i) {
        if (s.state() == expected) return;
        if (s.state() == State::Error) { std::cerr << s.message() << '\n'; std::abort(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cerr << "Timeout: " << s.message() << '\n'; std::abort();
}
int main() {
    // Exact frame boundaries across ten hours, including non-divisible rates.
    for (int rate : {44100, 48000, 96000, 22050, 48001}) {
        for (int64_t frame = -100; frame < 60 * 60 * 10 * 60LL; ++frame) {
            auto sample = frameSample(frame, rate);
            assert(sample * 60 >= frame * rate);
            assert(sample * 60 - frame * rate < 60);
        }
    }
    auto history = std::make_unique<CvHistory>();
    for (int64_t n = 0; n < 100000; ++n) {
        std::array<float, 16> cv{}; cv[0] = float(n); history->write(n, 48000, cv);
    }
    int64_t actual;
    assert(history->read(99000, actual)[0] == 99000 && actual == 99000);
    assert(history->read(0, actual)[0] == 100000 - CvHistory::CAPACITY);
    assert(actual == 100000 - CvHistory::CAPACITY);
    std::array<float, 16> reset{}; reset[0] = -2;
    history->write(100000, 44100, reset);
    assert(history->read(99000, actual)[0] == -2 && actual == 100000);
    SpscQueue<int, 17> q;
    std::thread producer([&] {
        for (int i = 0; i < 100000; ++i) {
            int* p; while (!(p = q.reserve())) std::this_thread::yield();
            *p = i; q.commit();
        }
    });
    for (int i = 0; i < 100000; ++i) {
        int* p; while (!(p = q.front())) std::this_thread::yield();
        assert(*p == i); q.pop();
    }
    producer.join();
    std::string dir = "build/test-takes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    CaptureSession s;
    Config c; c.directory = dir; c.sampleRate = 48000; c.names[0] = "comma, quote\" column";
    Config bad = c; bad.names[0] = "frame"; assert(!s.arm(bad));
    bad = c; bad.names[0] = bad.names[1]; assert(!s.arm(bad));
    bad = c; bad.names[0].clear(); assert(!s.arm(bad));
    bad = c; bad.cvDelayFrames = -1; assert(!s.arm(bad));
    bad = c; bad.cvDelayFrames = 5; assert(!s.arm(bad));
    assert(s.arm(c)); waitFor(s, State::Priming);
    std::vector<uint8_t> image(512 * 512 * 4, 0);
    for (size_t i = 3; i < image.size(); i += 4) image[i] = 255;
    assert(s.submitPixels(image.data(), 9200)); waitFor(s, State::Ready);
    assert(s.begin(10000));
    // Feed real sample counts but permit the worker to experience variable delays.
    for (int frame = 0; frame < 120; ++frame) {
        std::array<float, 16> cv{};
        for (int ch = 0; ch < 16; ++ch) cv[ch] = frame * 0.125f + ch;
        assert(s.frame(frame, 10000 + frame * 800, cv));
        for (int n = frame * 800; n < (frame + 1) * 800; ++n)
            assert(s.audio(10000 + n, .5f * std::sin(n * 2 * 3.141592653589793 * 440 / 48000), .25f * std::sin(n * 2 * 3.141592653589793 * 880 / 48000)));
        for (size_t i = 0; i < image.size(); i += 4)
            image[i] = image[i + 1] = image[i + 2] = frame % 2 ? 250 : 10;
        // Cross the onset by one sample in alternating directions. Nearest-read
        // selection must preserve every alternating image without a false repeat.
        while (!s.submitPixels(image.data(), 10000 + frame * 800 + (frame % 2 ? -1 : 1))) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(frame % 3 + 1));
    }
    s.end(10000 + 120 * 800);
    for (int i = 0; i < 10000 && !s.completed(); ++i) {
        if (s.state() == State::Error) { std::cerr << s.message() << '\n'; return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(s.completed() == 1);
    std::string first = s.lastTake();
    // Auto re-arm must not carry CV or pixels over from the previous take.
    waitFor(s, State::Priming);
    assert(s.submitPixels(image.data(), 200000)); waitFor(s, State::Ready);
    assert(s.begin(200100));
    std::array<float, 16> cv{}; cv[0] = -7.25f;
    assert(s.frame(0, 200100, cv));
    for (int n = 200100; n < 200500; ++n) assert(s.audio(n, .2f, .2f));
    s.end(200500);
    for (int i = 0; i < 10000 && s.completed() != 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.completed() == 2);
    s.disarm(); waitFor(s, State::Idle);
    // Invalid voltage must fail the take, preserve diagnostics, and publish no final pair.
    assert(s.arm(c)); waitFor(s, State::Priming);
    assert(s.submitPixels(image.data(), 300000)); waitFor(s, State::Ready);
    assert(s.begin(300100)); cv[0] = std::numeric_limits<float>::quiet_NaN();
    assert(s.frame(0, 300100, cv)); s.end(300500);
    for (int i = 0; i < 10000 && s.state() != State::Error; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.state() == State::Error); assert(s.completed() == 2);
    assert(s.message().find("not finite") != std::string::npos);
    assert(!fs::exists(fs::path(dir) / "take-0003.mp4"));
    assert(!fs::exists(fs::path(dir) / "take-0003.csv"));
    assert(fs::exists(fs::path(dir) / "take-0003.json"));
    // Delay changes CSV only. Use identical video/audio with zero/two-frame
    // correction, at a sample rate not divisible by 60 or by the output rate.
    c.sampleRate = 48001;
    for (int delay : {0, 2}) {
        c.prefix = "delay" + std::to_string(delay); c.cvDelayFrames = delay;
        assert(s.arm(c)); waitFor(s, State::Priming);
        int64_t origin = 400000;
        assert(s.submitPixels(image.data(), origin - 800)); waitFor(s, State::Ready);
        assert(s.begin(origin)); s.disarm();
        uint64_t previous = s.completed();
        for (int i = 0; i < 30; ++i) {
            for (int ch = 0; ch < 16; ++ch) cv[ch] = float(i - delay + ch);
            assert(s.frame(i, origin + frameSample(i, c.sampleRate), cv, origin + frameSample(i - delay, c.sampleRate)));
            for (int64_t n = frameSample(i, c.sampleRate); n < frameSample(i + 1, c.sampleRate); ++n)
                assert(s.audio(origin + n, .5f * std::sin(n * 2 * 3.141592653589793 * 440 / c.sampleRate), .25f * std::sin(n * 2 * 3.141592653589793 * 880 / c.sampleRate)));
            for (size_t pixel = 0; pixel < image.size(); pixel += 4)
                image[pixel] = image[pixel + 1] = image[pixel + 2] = 40 + i * 4;
            while (!s.submitPixels(image.data(), origin + frameSample(i, c.sampleRate) + 1))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        s.end(origin + frameSample(30, c.sampleRate));
        assert(!s.wantsPixels()); // No post-roll; audio/video retain the gate interval.
        waitFor(s, State::Idle); assert(s.completed() == previous + 1);
    }
    // A missing audio sample must fail explicitly, never compress the timeline.
    c.prefix = "missing-audio"; c.cvDelayFrames = 0;
    assert(s.arm(c)); waitFor(s, State::Priming);
    assert(s.submitPixels(image.data(), 600000)); waitFor(s, State::Ready);
    assert(s.begin(600100)); assert(s.frame(0, 600100, cv));
    assert(s.audio(600100, 0, 0)); s.end(600200);
    for (int i = 0; i < 10000 && s.state() != State::Error; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.state() == State::Error); assert(s.message().find("Audio sample count") != std::string::npos);
    assert(!fs::exists(fs::path(dir) / "missing-audio-0001.mp4"));
    c.prefix = "invalid-audio";
    assert(s.arm(c)); waitFor(s, State::Priming);
    assert(s.submitPixels(image.data(), 700000)); waitFor(s, State::Ready);
    assert(s.begin(700100)); assert(s.frame(0, 700100, cv));
    assert(s.audio(700100, std::numeric_limits<float>::infinity(), 0)); s.end(700101);
    for (int i = 0; i < 10000 && s.state() != State::Error; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(s.state() == State::Error); assert(s.message().find("audio input is not finite") != std::string::npos);
    assert(!fs::exists(fs::path(dir) / "invalid-audio-0001.mp4"));
    s.shutdown();
    std::ofstream pointer("build/last-test-directory.txt"); pointer << fs::absolute(dir).string();
    std::cout << "Scheduler, queues, audio boundaries, CSV correction, and paired MP4 recording passed.\n" << fs::absolute(dir) << '\n';
}
