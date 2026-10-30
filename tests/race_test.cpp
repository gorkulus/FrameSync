// Deterministic scheduling of the real CaptureSession, including FFmpeg output.
// Only this executable is compiled with FRAMESYNC_TEST_HOOKS.
#include "CaptureSession.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <cstdlib>

using namespace framesync;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Hooks {
    struct Gate { bool armed = false, reached = false, released = false, value = false; };
    std::array<Gate, size_t(TestPoint::Count)> gates{};
    std::mutex mutex;
    std::condition_variable cv;
    TestHooks api{this, [](void* p, TestPoint point, bool value) { static_cast<Hooks*>(p)->call(point, value); }};
    [[noreturn]] static void timeout(const char* where) {
        std::cerr << "HOOK TIMEOUT: " << where << std::endl; std::exit(2);
    }
    void call(TestPoint point, bool value) {
        std::unique_lock<std::mutex> lock(mutex);
        auto& g = gates[size_t(point)];
        cv.notify_all();
        if (!g.armed) return;
        g.armed = false; g.reached = true; g.value = value; cv.notify_all();
        if (!cv.wait_for(lock, 10s, [&] { return g.released; })) timeout("callback release");
    }
    void gate(TestPoint point) {
        std::lock_guard<std::mutex> lock(mutex);
        gates[size_t(point)] = {true, false, false, false};
    }
    bool reached(TestPoint point) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 10s, [&] { return gates[size_t(point)].reached; })) timeout("gate arrival");
        return gates[size_t(point)].value;
    }
    void release(TestPoint point) {
        std::lock_guard<std::mutex> lock(mutex);
        gates[size_t(point)].released = true; cv.notify_all();
    }
    void state(CaptureSession& s, State expected) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, 10s, [&] { return s.state() == expected; })) {
            std::cerr << "State " << int(s.state()) << ": " << s.message() << '\n';
            timeout("session state");
        }
    }
};

struct Fixture {
    Hooks hooks;
    CaptureSession session{&hooks.api};
    Config config;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(512 * 512 * 4, 120);
    explicit Fixture(const fs::path& dir) { config.directory = dir.u8string(); }
    void ready() {
        if (!session.arm(config)) Hooks::timeout("arm rejected");
        hooks.state(session, State::Priming);
        if (!session.submitPixels(pixels.data(), 10000)) Hooks::timeout("priming pixels rejected");
        hooks.state(session, State::Ready);
    }
    // Engine producer only, excluding end's sample. CV marks the current take.
    bool samples(int64_t origin = 10000, int count = 1601, float marker = 3.25f) {
        bool ok = true;
        std::array<float, 16> cv{}; cv[0] = marker;
        for (int n = 0; n < count; ++n) {
            if (n % 800 == 0) ok = session.frame(n / 800, origin + n, cv) && ok;
            ok = session.audio(origin + n, .125f, -.25f) && ok;
        }
        session.end(origin + count);
        return ok;
    }
    bool freshTake() {
        config.prefix = "fresh";
        ready();
        bool ok = session.begin(20000);
        session.disarm();
        ok = samples(20000, 800, -7.5f) && ok;
        hooks.state(session, State::Idle);
        return ok;
    }
};

bool acceptedRace(const fs::path& dir, const std::string& mode) {
    Fixture f(dir); f.ready();
    auto& h = f.hooks; auto& s = f.session;
    // Freeze the worker just AFTER draining events, before it can decide to cancel.
    h.gate(TestPoint::EventsDrained); h.reached(TestPoint::EventsDrained);
    bool ok = true, canceled = false;
    if (mode == "published-before-start") {
        h.gate(TestPoint::RecordingPublished);
        auto engine = std::async(std::launch::async, [&] {
            bool accepted = s.begin(10000);
            return f.samples() && accepted;
        });
        h.reached(TestPoint::RecordingPublished); // Recording visible, Start absent.
        s.disarm();
        h.gate(TestPoint::CancellationChecked);
        h.release(TestPoint::EventsDrained);
        canceled = h.reached(TestPoint::CancellationChecked);
        h.release(TestPoint::RecordingPublished);
        ok = engine.get();
        h.release(TestPoint::CancellationChecked);
    } else if (mode == "stale-ready-snapshot") {
        // begin already saw keepArmed=true. Worker sees Ready after disarm.
        // Let begin win between the worker's snapshot and cancellation CAS.
        h.gate(TestPoint::BeginChecked);
        auto engine = std::async(std::launch::async, [&] {
            bool accepted = s.begin(10000);
            return f.samples() && accepted;
        });
        h.reached(TestPoint::BeginChecked); s.disarm();
        h.gate(TestPoint::BeforeCancelClaim); h.gate(TestPoint::CancellationChecked);
        h.release(TestPoint::EventsDrained); h.reached(TestPoint::BeforeCancelClaim);
        h.release(TestPoint::BeginChecked); ok = engine.get(); // Already Finishing.
        h.release(TestPoint::BeforeCancelClaim);
        canceled = h.reached(TestPoint::CancellationChecked);
        h.release(TestPoint::CancellationChecked);
    } else {
        ok = s.begin(10000); s.disarm(); // Start visible only AFTER drain completed.
        if (mode == "drained-before-finishing") ok = f.samples() && ok;
        h.gate(TestPoint::CancellationChecked);
        h.release(TestPoint::EventsDrained);
        canceled = h.reached(TestPoint::CancellationChecked);
        if (mode == "drained-before-recording") ok = f.samples() && ok;
        h.release(TestPoint::CancellationChecked);
    }
    h.state(s, State::Idle);
    bool saved = s.completed() == 1 && fs::exists(fs::u8path(s.lastTake() + ".mp4"));
    if (!ok || canceled || !saved) {
        std::cerr << "REGRESSION FAILED: " << mode << " accepted take lost/canceled; completed=" << s.completed() << '\n';
        return false;
    }
    // No auto-rearm; a rejected idle trigger cannot queue a stale Start.
    if (s.begin(19000) || !f.freshTake() || s.completed() != 2) return false;
    s.shutdown();
    return true;
}

bool cancellationWins(const fs::path& dir) {
    Fixture f(dir); f.ready();
    auto& h = f.hooks; auto& s = f.session;
    h.gate(TestPoint::EventsDrained); h.reached(TestPoint::EventsDrained);
    h.gate(TestPoint::BeginChecked);
    auto engine = std::async(std::launch::async, [&] { return s.begin(10000); });
    h.reached(TestPoint::BeginChecked); s.disarm();
    h.gate(TestPoint::CancellationChecked); h.release(TestPoint::EventsDrained);
    bool canceled = h.reached(TestPoint::CancellationChecked);
    h.release(TestPoint::BeginChecked); bool accepted = engine.get();
    h.release(TestPoint::CancellationChecked); h.state(s, State::Idle);
    bool empty = fs::is_empty(fs::u8path(f.config.directory));
    return canceled && !accepted && empty && f.freshTake() && s.completed() == 1;
}

bool noStart(const fs::path& dir) {
    Fixture f(dir); f.ready();
    f.session.disarm();
    bool rejected = !f.session.begin(10000);
    f.hooks.state(f.session, State::Idle);
    bool empty = fs::is_empty(dir);
    return rejected && empty && f.freshTake() && f.session.completed() == 1;
}

bool shutdownCase(const fs::path& dir, bool claimed) {
    Fixture f(dir); f.ready();
    auto& h = f.hooks; auto& s = f.session;
    h.gate(TestPoint::EventsDrained); h.reached(TestPoint::EventsDrained);
    std::future<bool> engine;
    if (claimed) {
        h.gate(TestPoint::RecordingPublished);
        engine = std::async(std::launch::async, [&] { return s.begin(10000); });
        h.reached(TestPoint::RecordingPublished);
    }
    h.gate(TestPoint::CleanupClaimed);
    auto shutdown = std::async(std::launch::async, [&] { s.shutdown(); });
    // disarm and quit are set by shutdown, but the worker remains at its drain hook.
    h.release(TestPoint::EventsDrained); h.reached(TestPoint::CleanupClaimed);
    bool accepted = true;
    if (claimed) { h.release(TestPoint::RecordingPublished); accepted = engine.get(); }
    h.release(TestPoint::CleanupClaimed);
    if (shutdown.wait_for(10s) != std::future_status::ready) Hooks::timeout("shutdown");
    shutdown.get();
    if (s.completed() || s.arm(f.config) || s.begin(10000)) return false;
    if (!claimed) return s.state() == State::Idle && fs::is_empty(dir);
    return accepted && s.state() == State::Error && fs::exists(dir / "take-0001.json")
        && fs::exists(dir / "take-0001.partial.csv") && !fs::exists(dir / "take-0001.mp4");
}

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    fs::path root = "build/race-takes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    for (const char* mode : {"published-before-start", "drained-before-recording", "drained-before-finishing", "stale-ready-snapshot"}) {
        if (selected != "all" && selected != mode) continue;
        if (!acceptedRace(root / mode, mode)) return 1;
        std::cout << "PASS: " << mode << '\n';
    }
    if (selected == "all") {
        if (!cancellationWins(root / "cancel-wins") || !noStart(root / "no-start")
            || !shutdownCase(root / "shutdown-idle", false) || !shutdownCase(root / "shutdown-claimed", true)) return 1;
        std::cout << "PASS: cancellation wins, disarm before start, pending-Start shutdown, fresh generations\n";
    }
    std::ofstream("build/last-race-directory.txt") << fs::absolute(root).u8string();
}
