// Exercise the actual module processor and Rack's trigger detector, without a GUI.
#include "RecorderModule.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
static_assert(FrameSync::GATE == 0 && FrameSync::START == 1 && FrameSync::STOP == 2);
static_assert(FrameSync::POLY == 3 && FrameSync::CV == 4 && FrameSync::TOGGLE == 20);
static_assert(FrameSync::NUM_INPUTS == 23 && FrameSync::AUDIO_L == 21 && FrameSync::AUDIO_R == 22);

struct Fixture {
    FrameSync module;
    int64_t sample = 0, stereoOrigin = 0;
    bool stereo = false;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(512 * 512 * 4, 40);
    Fixture() {
        for (int id : {FrameSync::START, FrameSync::STOP, FrameSync::TOGGLE}) module.inputs[id].channels = 1;
        for (int i = 0; i < 16; ++i) { module.inputs[FrameSync::CV + i].channels = 1; module.inputs[FrameSync::CV + i].setVoltage(i + .25f); }
        tick(); // Initialize edge detectors with low inputs.
    }
    void voltage(int port, float value) { module.inputs[port].setVoltage(value); }
    void tick() {
        if (stereo) {
            module.inputs[FrameSync::AUDIO_L].channels = 2;
            module.inputs[FrameSync::AUDIO_R].channels = 1;
            double phase = (sample - stereoOrigin) * 2 * 3.141592653589793 / 48000;
            module.inputs[FrameSync::AUDIO_L].setVoltage(2.f * std::sin(440 * phase), 0);
            module.inputs[FrameSync::AUDIO_L].setVoltage(3.f * std::sin(440 * phase), 1);
            module.inputs[FrameSync::AUDIO_R].setVoltage(2.5f * std::sin(880 * phase));
        }
        module.process({48000.f, 1.f / 48000.f, sample++});
    }
    void wait(State state) {
        for (int i = 0; i < 10000; ++i) {
            if (module.capture.state() == state) return;
            if (module.capture.state() == State::Error) { std::cerr << module.capture.message() << '\n'; std::abort(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cerr << "Timeout: " << module.capture.message() << '\n'; std::abort();
    }
    void ready() {
        wait(State::Priming);
        assert(module.capture.submitPixels(pixels.data(), sample - 1));
        wait(State::Ready);
    }
    void arm(const fs::path& directory, int correction = 0) {
        framesync::Config config; config.directory = directory.string(); config.cvDelayFrames = correction;
        module.armedCvDelay = correction;
        assert(module.capture.arm(config)); ready();
    }
};

static void verifyTake(const fs::path& directory, int index, int64_t samples, int rows) {
    auto base = directory / ("take-000" + std::to_string(index));
    json_error_t error;
    json_t* j = json_load_file((base.string() + ".json").c_str(), 0, &error);
    assert(j && std::string(json_string_value(json_object_get(j, "status"))) == "complete");
    assert(json_integer_value(json_object_get(j, "recorded_engine_samples")) == samples);
    assert(json_integer_value(json_object_get(j, "frame_count")) == rows);
    json_decref(j);
    std::ifstream csv(base.string() + ".csv"); std::string line;
    int lines = 0; while (std::getline(csv, line)) ++lines;
    assert(lines == rows + 1);
}

int main() {
    fs::path directory = "build/control-takes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        Fixture f;
        // A pulse received while disarmed, even held through arming, is ignored.
        f.voltage(FrameSync::TOGGLE, 10); f.tick();
        f.arm(directory / "bar"); f.tick();
        assert(f.module.capture.state() == State::Ready);
        f.voltage(FrameSync::TOGGLE, 0); f.tick();
        // Same-sample START and STOP leave a ready recorder waiting.
        f.voltage(FrameSync::START, 10); f.voltage(FrameSync::STOP, 10); f.tick();
        assert(f.module.capture.state() == State::Ready);
        f.voltage(FrameSync::START, 0); f.voltage(FrameSync::STOP, 0); f.tick();
        // STOP also wins against TOGGLE; no delayed start on the following sample.
        f.voltage(FrameSync::TOGGLE, 10); f.voltage(FrameSync::STOP, 10); f.tick();
        f.voltage(FrameSync::STOP, 0); f.tick();
        assert(f.module.capture.state() == State::Ready);
        f.voltage(FrameSync::TOGGLE, 0); f.tick();
        // First bar pulse starts. Holding it high never causes multiple toggles.
        int64_t onset = f.sample; f.stereo = true; f.stereoOrigin = onset;
        f.voltage(FrameSync::TOGGLE, 10); f.tick();
        assert(f.module.capture.state() == State::Recording);
        for (int i = 0; i < 100; ++i) f.tick();
        assert(f.module.capture.state() == State::Recording);
        f.voltage(FrameSync::TOGGLE, 0);
        while (f.sample < onset + 96000) f.tick();
        f.voltage(FrameSync::TOGGLE, 10); f.tick();
        assert(f.module.capture.state() == State::Finishing);
        f.ready(); f.tick();
        assert(f.module.capture.completed() == 1 && f.module.capture.state() == State::Ready);
        verifyTake(directory / "bar", 1, 96000, 120); // One 4/4 bar at 120 BPM.
        f.module.capture.disarm(); f.wait(State::Idle);
    }
    {
        Fixture f; f.arm(directory / "corrected", 2);
        int64_t onset = f.sample;
        // A GUI REC click and TOGGLE edge on the same sample toggle only once.
        f.module.command = 3; f.voltage(FrameSync::TOGGLE, 10); f.tick();
        assert(f.module.capture.state() == State::Recording);
        f.voltage(FrameSync::TOGGLE, 0);
        while (f.sample < onset + 1600) f.tick();
        f.voltage(FrameSync::TOGGLE, 10); f.tick();
        assert(f.module.capture.state() == State::Finishing);
        // Edges during saving are consumed, not queued for the next take.
        f.voltage(FrameSync::TOGGLE, 0); f.tick();
        f.voltage(FrameSync::TOGGLE, 10); f.tick();
        assert(f.module.capture.state() == State::Finishing);
        assert(!f.module.capture.wantsPixels());
        f.ready(); f.tick();
        assert(f.module.capture.completed() == 1 && f.module.capture.state() == State::Ready);
        verifyTake(directory / "corrected", 1, 1600, 2);
        // A fresh edge starts another take; simultaneous START/STOP stops it.
        f.voltage(FrameSync::TOGGLE, 0); f.tick();
        onset = f.sample;
        f.voltage(FrameSync::TOGGLE, 10); f.tick();
        assert(f.module.capture.state() == State::Recording);
        f.voltage(FrameSync::START, 10); f.voltage(FrameSync::STOP, 10); f.tick();
        assert(f.module.capture.state() == State::Finishing);
        assert(!f.module.capture.wantsPixels());
        f.ready();
        verifyTake(directory / "corrected", 2, 1, 1);
        f.module.capture.disarm(); f.wait(State::Idle);
    }
    // Use the real module processor to check pre-trigger history, mono routing,
    // and polyphonic audio sums. The GUI pixels are deliberately held constant.
    for (int delay : {0, 2}) {
        Fixture f;
        f.module.inputs[FrameSync::AUDIO_L].channels = 2;
        f.module.inputs[FrameSync::AUDIO_R].channels = 0;
        auto tick = [&] {
            float tone = std::sin(float(f.sample) * (2.f * 3.141592653589793f * 440 / 48000));
            f.module.inputs[FrameSync::AUDIO_L].setVoltage(2.f * tone, 0);
            f.module.inputs[FrameSync::AUDIO_L].setVoltage(3.f * tone, 1);
            f.voltage(FrameSync::CV, f.sample * .001f);
            f.tick();
        };
        while (f.sample < 4800) tick();
        f.arm(directory / ("mono-delay" + std::to_string(delay)), delay);
        f.voltage(FrameSync::TOGGLE, 10); tick();
        f.voltage(FrameSync::TOGGLE, 0);
        while (f.sample < 4800 + 24000) tick();
        f.voltage(FrameSync::TOGGLE, 10); tick();
        f.ready(); verifyTake(directory / ("mono-delay" + std::to_string(delay)), 1, 24000, 30);
        f.module.capture.disarm(); f.wait(State::Idle);
    }
    {
        Fixture f;
        json_t* legacy = json_pack("{s:i}", "videoAdvanceFrames", 2);
        f.module.dataFromJson(legacy); json_decref(legacy);
        assert(f.module.config.cvDelayFrames == 2);
        json_t* saved = f.module.dataToJson();
        assert(json_integer_value(json_object_get(saved, "cvDelayFrames")) == 2);
        assert(!json_object_get(saved, "videoAdvanceFrames"));
        json_object_set_new(saved, "videoAdvanceFrames", json_integer(4));
        f.module.dataFromJson(saved); json_decref(saved);
        assert(f.module.config.cvDelayFrames == 2); // New field takes precedence.
    }
    {
        // Exercise defaults and JSON overrides in a separate temporary Rack user
        // directory; never read or write the installed profile's configuration.
        std::string oldUserDir = asset::userDir;
        auto profile = directory / "encoder profile";
        fs::create_directories(profile);
        asset::userDir = fs::absolute(profile).u8string();
        const std::string defaultPath = u8"C:/encoder space ü/ffmpeg.exe";
        std::ofstream(profile / "FrameSync-ffmpeg.txt", std::ios::binary) << defaultPath << "\r\nignored second line";
        {
            FrameSync module;
            assert(module.config.ffmpegPath == defaultPath);
            json_t* data = json_object();
            module.dataFromJson(data); // Old patches preserve the constructor default.
            assert(module.config.ffmpegPath == defaultPath);
            json_object_set_new(data, "ffmpegPath", json_string("explicit executable"));
            module.dataFromJson(data);
            assert(module.config.ffmpegPath == "explicit executable");
            json_t* saved = module.dataToJson();
            assert(std::string(json_string_value(json_object_get(saved, "ffmpegPath"))) == "explicit executable");
            json_decref(saved);
            json_object_set_new(data, "ffmpegPath", json_string(""));
            module.dataFromJson(data);
            assert(module.config.ffmpegPath.empty());
            json_decref(data);
        }
        asset::userDir = oldUserDir;
    }
    std::ofstream("build/last-control-test-directory.txt") << fs::absolute(directory).string();
    std::cout << "Module control tests passed: bar toggling, held triggers, STOP priority, busy edges, CV history, mono/poly audio routing, migration, and stable input IDs.\n";
}
