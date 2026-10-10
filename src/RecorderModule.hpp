// FrameSync recorder engine and control inputs. GPL-3.0-or-later.
#pragma once
#include "plugin.hpp"
#include "CaptureSession.hpp"
#include <osdialog.h>
#include <algorithm>
#include <cmath>

using framesync::State;

struct FrameSync : Module {
    // Append new ports so saved patches retain all existing cable/input IDs.
    enum InputIds { GATE, START, STOP, POLY, CV, TOGGLE = CV + 16, AUDIO_L, AUDIO_R, NUM_INPUTS };
    enum OutputIds { READY, RECORDING, DONE, NUM_OUTPUTS };
    framesync::CaptureSession capture;
    framesync::Config config;
    bool regionSet = false;
    bool selecting = false;
    std::atomic<int> command{0}; // GUI -> DSP: 1 start, 2 stop, 3 toggle
    std::atomic<bool> rateError{false};
    std::atomic<int> armedRate{48000};
    std::atomic<int> armedCvDelay{0};
    std::unique_ptr<framesync::CvHistory> cvHistory{new framesync::CvHistory};
    std::atomic<int64_t> latestSample{0};
    dsp::SchmittTrigger startTrigger, stopTrigger, gateTrigger, toggleTrigger;
    dsp::PulseGenerator donePulse;
    int64_t origin = 0, nextFrame = 0;
    bool localRecording = false;
    uint64_t completedSeen = 0;
#ifdef FRAMESYNC_TEST_HARNESS
    bool testArmOnLoad = false;
#endif

    FrameSync() {
        Module::config(0, NUM_INPUTS, NUM_OUTPUTS, 0);
        configInput(GATE, "Record gate (high records, low stops)");
        configInput(START, "Start recording"); configInput(STOP, "Stop recording");
        configInput(TOGGLE, "Toggle recording (one rising edge starts; the next stops)");
        configInput(AUDIO_L, "Audio left / mono (10 V = full scale)");
        configInput(AUDIO_R, "Audio right (unpatched copies left)");
        configInput(POLY, "CV channels 1–16 (individual jacks override)");
        for (int i = 0; i < 16; ++i) configInput(CV + i, string::f("CV %02d", i + 1));
        configOutput(READY, "Ready for a take"); configOutput(RECORDING, "Recording"); configOutput(DONE, "Take saved (trigger)");
    }
    ~FrameSync() override { capture.shutdown(); }
    void process(const ProcessArgs& args) override {
        latestSample.store(args.frame, std::memory_order_relaxed);
        std::array<float, 16> values{};
        for (int i = 0; i < 16; ++i)
            values[i] = inputs[CV + i].isConnected() ? inputs[CV + i].getVoltage() : inputs[POLY].getVoltage(i);
        cvHistory->write(args.frame, int(std::lround(args.sampleRate)), values);
        bool risingGate = gateTrigger.process(inputs[GATE].getVoltage(), 0.1f, 2.f);
        bool start = startTrigger.process(inputs[START].getVoltage(), 0.1f, 2.f) || risingGate;
        bool stop = stopTrigger.process(inputs[STOP].getVoltage(), 0.1f, 2.f);
        // Consume edges in every state: an ignored busy/disarmed pulse must not
        // become a delayed start when the recorder is ready again.
        bool toggle = toggleTrigger.process(inputs[TOGGLE].getVoltage(), 0.1f, 2.f);
        State state = capture.state();
        if (localRecording && state != State::Recording) localRecording = false;
        bool fallingGate = inputs[GATE].isConnected() && localRecording && inputs[GATE].getVoltage() <= 0.1f;
        int cmd = command.exchange(0);
        toggle = toggle || cmd == 3;
        if (cmd == 1 || (toggle && !localRecording)) start = true;
        if (cmd == 2 || (toggle && localRecording)) stop = true;
        if ((state == State::Ready || state == State::Recording) && int(std::lround(args.sampleRate)) != armedRate.load()) {
            rateError = true; start = false; stop = true;
        }
        if (localRecording && (stop || fallingGate)) {
            capture.end(args.frame); localRecording = false;
        }
        if (start && !stop && !localRecording && capture.begin(args.frame)) {
            origin = args.frame; nextFrame = 0; localRecording = true;
        }
        if (localRecording) {
            // Match VCV Recorder's input scaling and left-to-right normalization.
            float left = inputs[AUDIO_L].getVoltageSum() / 10.f;
            float right = inputs[AUDIO_R].isConnected() ? inputs[AUDIO_R].getVoltageSum() / 10.f : left;
            capture.audio(args.frame, left, right);
            if (args.frame - origin >= framesync::frameSample(nextFrame, armedRate.load())) {
                int64_t requested = origin + framesync::frameSample(nextFrame - armedCvDelay.load(), armedRate.load());
                int64_t actual;
                auto cv = cvHistory->read(requested, actual);
                capture.frame(nextFrame++, args.frame, cv, actual, actual != requested);
            }
        }
        uint64_t completed = capture.completed();
        if (completed != completedSeen) { completedSeen = completed; donePulse.trigger(0.005f); }
        state = capture.state();
        outputs[READY].setVoltage(state == State::Ready ? 10.f : 0.f);
        outputs[RECORDING].setVoltage(state == State::Recording ? 10.f : 0.f);
        outputs[DONE].setVoltage(donePulse.process(args.sampleTime) ? 10.f : 0.f);
    }
    bool active() const {
        State s = capture.state(); return s != State::Idle && s != State::Error;
    }
    void chooseFolder() {
        if (active()) return;
        char* path = osdialog_file(OSDIALOG_OPEN_DIR, config.directory.empty() ? nullptr : config.directory.c_str(), nullptr, nullptr);
        if (path) { config.directory = path; std::free(path); }
    }
    void arm() {
        if (capture.state() == State::Recording) { command = 2; capture.disarm(); return; }
        if (active()) { capture.disarm(); return; }
        if (config.directory.empty()) chooseFolder();
        if (config.directory.empty() || !regionSet || selecting) return;
        config.sampleRate = int(std::lround(APP->engine->getSampleRate()));
        armedRate = config.sampleRate; armedCvDelay = config.cvDelayFrames; rateError = false;
        capture.arm(config);
    }
    json_t* dataToJson() override {
        json_t* j = json_object();
        json_object_set_new(j, "directory", json_string(config.directory.c_str()));
        json_object_set_new(j, "prefix", json_string(config.prefix.c_str()));
        json_object_set_new(j, "size", json_integer(config.size));
        json_object_set_new(j, "cvDelayFrames", json_integer(config.cvDelayFrames));
        json_object_set_new(j, "x", json_integer(config.x)); json_object_set_new(j, "y", json_integer(config.y));
        json_object_set_new(j, "regionSet", json_boolean(regionSet));
        json_t* names = json_array();
        for (const auto& name : config.names) json_array_append_new(names, json_string(name.c_str()));
        json_object_set_new(j, "names", names);
        return j;
    }
    void dataFromJson(json_t* j) override {
#ifdef FRAMESYNC_TEST_HARNESS
        testArmOnLoad = json_is_true(json_object_get(j, "testArmOnLoad"));
#endif
        if (json_is_string(json_object_get(j, "directory"))) config.directory = json_string_value(json_object_get(j, "directory"));
        if (json_is_string(json_object_get(j, "prefix"))) config.prefix = json_string_value(json_object_get(j, "prefix"));
        config.size = json_integer_value(json_object_get(j, "size")) == 1024 ? 1024 : 512;
        // Preserve the saved amount from versions 2.0.x, now applied to CSV only.
        json_t* delay = json_object_get(j, "cvDelayFrames");
        if (!delay) delay = json_object_get(j, "videoAdvanceFrames");
        config.cvDelayFrames = int(std::clamp<json_int_t>(json_integer_value(delay), 0, 4));
        config.x = std::max<int>(0, json_integer_value(json_object_get(j, "x")));
        config.y = std::max<int>(0, json_integer_value(json_object_get(j, "y")));
        regionSet = json_is_true(json_object_get(j, "regionSet"));
        json_t* names = json_object_get(j, "names");
        for (int i = 0; i < 16; ++i) if (json_is_string(json_array_get(names, i))) config.names[i] = json_string_value(json_array_get(names, i));
        // Deliberately never resume an armed/recording session when loading a patch.
    }
};
