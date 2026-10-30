// Windows path, subprocess failure, and bounded cancellation integration tests.
#include "Encoder.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <thread>

using namespace framesync;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static void wait(CaptureSession& session, State expected) {
    auto deadline = Clock::now() + std::chrono::seconds(15);
    while (session.state() != expected) {
        if (session.state() == State::Error && expected != State::Error) {
            std::cerr << session.message() << '\n'; std::abort();
        }
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main(int argc, char**) {
    if (argc > 1) {
        if (std::string(std::getenv("FRAMESYNC_FAKE_MODE")) == "exit") return 7;
        Sleep(600000); return 0; // Intentionally never reads stdin; parent must cancel.
    }
    assert(winEncoder::quote(L"space \\ \" tail\\") == L"\"space \\ \\\" tail\\\\\"");
    auto directory = utf8Path("build/Windows takes \xc3\xbc-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    Config config;
    config.directory = directory.u8string();
    auto encoderCopy = directory / utf8Path("encoder space \xc3\xbc.exe");
    fs::copy_file(utf8Path(std::getenv("FRAMESYNC_FFMPEG")), encoderCopy);
    config.ffmpegPath = fs::absolute(encoderCopy).u8string();
    for (int size : {512, 1024}) {
        config.size = size; config.prefix = "size" + std::to_string(size);
        // Simultaneous reservations must choose distinct take numbers.
        CaptureSession a, b;
        assert(a.arm(config) && b.arm(config)); wait(a, State::Priming); wait(b, State::Priming);
        std::vector<uint8_t> image(size_t(size) * size * 4, 120);
        for (auto* session : {&a, &b}) {
            assert(session->submitPixels(image.data(), 1000)); wait(*session, State::Ready);
            assert(session->begin(1000)); session->disarm();
            std::array<float, 16> cv{}; cv[0] = 3.25f;
            assert(session->frame(0, 1000, cv));
            for (int n = 0; n < 800; ++n) assert(session->audio(1000 + n, .1f, -.1f));
            session->end(1800); wait(*session, State::Idle); assert(session->completed() == 1);
        }
        assert(a.lastTake() != b.lastTake());
        assert(fs::exists(utf8Path(a.lastTake() + ".mp4")) && fs::exists(utf8Path(b.lastTake() + ".csv")));
    }
    config.prefix = "missing"; config.ffmpegPath = fs::absolute(directory / "absent.exe").u8string();
    { CaptureSession session; assert(session.arm(config)); wait(session, State::Error);
      assert(session.message().find("Cannot start FFmpeg") != std::string::npos); }
    wchar_t self[MAX_PATH]; assert(GetModuleFileNameW(nullptr, self, MAX_PATH));
    config.ffmpegPath = fs::path(self).u8string();
    std::vector<uint8_t> image(1024 * 1024 * 4);
    for (const char* mode : {"exit", "stall", "cancel"}) {
        SetEnvironmentVariableA("FRAMESYNC_FAKE_MODE", mode);
        // getenv's CRT snapshot also matters when this exe is used as the child.
        _putenv_s("FRAMESYNC_FAKE_MODE", mode);
        VideoEncoder encoder; std::atomic<bool> quit{false};
        encoder.open(config, (directory / (std::string(mode) + ".mp4")).u8string(),
            (directory / (std::string(mode) + ".log")).u8string());
        auto started = Clock::now();
        std::thread interrupter;
        if (std::string(mode) == "cancel") interrupter = std::thread([&] { Sleep(100); quit = true; });
        bool failed = false;
        try { encoder.write(image, quit); } catch (const std::exception&) { failed = true; }
        if (interrupter.joinable()) interrupter.join();
        encoder.cancel(); assert(failed);
        double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        assert(elapsed < (std::string(mode) == "stall" ? 12 : 3));
    }
    // CaptureSession shutdown also propagates quit while the worker is in WriteFile.
    config.prefix = "shutdown";
    { CaptureSession session; assert(session.arm(config)); wait(session, State::Priming);
      assert(session.submitPixels(image.data(), 1000)); wait(session, State::Ready);
      assert(session.begin(1000)); std::array<float, 16> cv{};
      assert(session.frame(0, 1000, cv)); assert(session.audio(1000, 0, 0));
      Sleep(100); auto started = Clock::now(); session.shutdown();
      assert(Clock::now() - started < std::chrono::seconds(3)); }
    _putenv_s("FRAMESYNC_FAKE_MODE", "");
    std::ofstream("build/last-windows-test-directory.txt") << fs::absolute(directory).u8string();
    std::cout << "Unicode/spaced executable and output paths, both sizes, exclusive reservations, missing/stopped/stalled encoders, and bounded shutdown passed.\n";
}
