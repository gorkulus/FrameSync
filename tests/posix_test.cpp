// Linux subprocess/path checks corresponding to the preserved Windows suite.
#include <cstring>
#include "Encoder.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <thread>

using namespace framesync;
namespace fs = std::filesystem;
static void wait(CaptureSession& s, State expected) {
    auto deadline = Clock::now() + std::chrono::seconds(15);
    while (s.state() != expected) {
        if (s.state() == State::Error && expected != State::Error) {
            std::cerr << s.message() << '\n'; std::abort();
        }
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main(int argc, char**) {
    if (argc > 1) {
        if (std::string(std::getenv("FRAMESYNC_FAKE_MODE")) == "exit") return 7;
        for (;;) pause(); // Deliberately never read stdin; owned child is canceled.
    }
    sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    const char* previous = std::getenv("FRAMESYNC_FFMPEG");
    bool hadPrevious = previous != nullptr;
    std::string oldValue = previous ? previous : "";
    Config c;
    unsetenv("FRAMESYNC_FFMPEG"); assert(encoderExecutable(c) == "/usr/bin/ffmpeg");
    setenv("FRAMESYNC_FFMPEG", "environment executable", 1);
    assert(encoderExecutable(c) == "environment executable");
    c.ffmpegPath = "module/default executable";
    assert(encoderExecutable(c) == "module/default executable");
    if (hadPrevious) setenv("FRAMESYNC_FFMPEG", oldValue.c_str(), 1); else unsetenv("FRAMESYNC_FFMPEG");

    auto directory = utf8Path(u8"build/Linux takes ü-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    auto executable = directory / utf8Path(u8"encoder space ü");
    fs::create_symlink("/usr/bin/ffmpeg", executable);
    c.directory = directory.u8string(); c.ffmpegPath = fs::absolute(executable).u8string();
    for (int size : {512, 1024}) {
        c.size = size; c.prefix = "size" + std::to_string(size);
        CaptureSession a, b;
        assert(a.arm(c) && b.arm(c)); wait(a, State::Priming); wait(b, State::Priming);
        std::vector<uint8_t> pixels(size_t(size) * size * 4, 120);
        for (auto* s : {&a, &b}) {
            assert(s->submitPixels(pixels.data(), 1000)); wait(*s, State::Ready);
            assert(s->begin(1000)); s->disarm();
            std::array<float, 16> cv{}; cv[0] = 3.25f;
            assert(s->frame(0, 1000, cv));
            for (int n = 0; n < 800; ++n) assert(s->audio(1000 + n, .1f, -.1f));
            s->end(1800); wait(*s, State::Idle); assert(s->completed() == 1);
        }
        assert(a.lastTake() != b.lastTake());
    }
    c.prefix = "missing"; c.ffmpegPath = fs::absolute(directory / "absent").u8string();
    { CaptureSession s; assert(s.arm(c)); wait(s, State::Error);
      assert(s.message().find("Cannot start FFmpeg") != std::string::npos); }
    c.ffmpegPath = fs::read_symlink("/proc/self/exe").u8string();
    std::vector<uint8_t> pixels(1024 * 1024 * 4);
    for (const char* mode : {"exit", "stall", "cancel"}) {
        setenv("FRAMESYNC_FAKE_MODE", mode, 1);
        VideoEncoder encoder; std::atomic<bool> quit{false};
        encoder.open(c, (directory / (std::string(mode) + ".mp4")).u8string(),
            (directory / (std::string(mode) + ".log")).u8string());
        auto start = Clock::now();
        std::thread interrupter;
        if (std::string(mode) == "cancel") interrupter = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); quit = true;
        });
        bool failed = false;
        try { encoder.write(pixels, quit); } catch (const std::exception&) { failed = true; }
        if (interrupter.joinable()) interrupter.join();
        encoder.cancel(); assert(failed);
        double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        assert(elapsed < (std::string(mode) == "stall" ? 12 : 3));
        std::cout << mode << ": " << elapsed << " seconds\n";
    }
    c.prefix = "shutdown"; setenv("FRAMESYNC_FAKE_MODE", "stall", 1);
    { CaptureSession s; assert(s.arm(c)); wait(s, State::Priming);
      assert(s.submitPixels(pixels.data(), 1000)); wait(s, State::Ready);
      assert(s.begin(1000)); std::array<float, 16> cv{}; assert(s.frame(0, 1000, cv));
      auto start = Clock::now(); s.shutdown();
      assert(std::chrono::duration<double>(Clock::now() - start).count() < 3); }
    unsetenv("FRAMESYNC_FAKE_MODE");
    std::ofstream("build/last-posix-test-directory.txt") << fs::absolute(directory).u8string();
    std::cout << "POSIX paths, reservations, encoder precedence/failure, timeout and shutdown passed\n";
}
