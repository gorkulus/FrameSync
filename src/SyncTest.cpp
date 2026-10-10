#include "plugin.hpp"
#include "Timeline.hpp"
#include <atomic>
#include <cmath>

struct SyncTest : Module {
    enum { RUN, NUM_PARAMS };
    enum { READY, NUM_INPUTS };
    enum { GATE, POLY, NUM_OUTPUTS };
    std::atomic<int> code{0};
    std::atomic<bool> running{false};
    dsp::SchmittTrigger runButton;
    bool waiting = false, autoRun = false, attempted = false;
    int64_t origin = 0;
    std::atomic<float> duration{5.f};
    SyncTest() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
        configButton(RUN, "Run one sync check when recorder is ready");
        configInput(READY, "Recorder READY"); configOutput(GATE, "Recorder GATE"); configOutput(POLY, "Test CV (16 channels)");
    }
    void process(const ProcessArgs& args) override {
        if (runButton.process(params[RUN].getValue()) || (autoRun && !attempted)) { waiting = true; attempted = true; }
        if (waiting && inputs[READY].getVoltage() >= 2.f && !running) { waiting = false; running = true; origin = args.frame; }
        int64_t elapsed = running ? args.frame - origin : 0;
        if (running && elapsed >= std::llround(duration.load() * args.sampleRate)) running = false;
        // Hold the final counter value through the recorder's correction post-roll.
        int n = running ? int(elapsed * 60 / int(std::lround(args.sampleRate))) % 4096 : code.load(std::memory_order_relaxed);
        code.store(n, std::memory_order_relaxed);
        outputs[GATE].setVoltage(running ? 10.f : 0.f);
        outputs[POLY].setChannels(16);
        outputs[POLY].setVoltage(float(n), 0);
        for (int i = 1; i < 16; ++i) outputs[POLY].setVoltage((n & (1 << ((i - 1) % 12))) ? 10.f : 0.f, i);
    }
    json_t* dataToJson() override {
        json_t* j = json_object(); json_object_set_new(j, "autoRun", json_boolean(autoRun));
        json_object_set_new(j, "duration", json_real(duration)); return j;
    }
    void dataFromJson(json_t* j) override {
        autoRun = json_is_true(json_object_get(j, "autoRun"));
        if (json_is_number(json_object_get(j, "duration"))) duration = clamp(float(json_number_value(json_object_get(j, "duration"))), 1.f, 60.f);
    }
};

struct TestDisplay : widget::Widget {
    SyncTest* module = nullptr;
    void draw(const DrawArgs& a) override {
        nvgBeginPath(a.vg); nvgRect(a.vg, 0, 0, box.size.x, box.size.y); nvgFillColor(a.vg, nvgRGB(0, 0, 0)); nvgFill(a.vg);
        int n = module ? module->code.load() : 0;
        // White corner markers and a 12-bit barcode allow automatic video decoding.
        nvgFillColor(a.vg, nvgRGB(255, 255, 255));
        nvgBeginPath(a.vg); nvgRect(a.vg, 8, 8, 8, 8); nvgRect(a.vg, 484, 8, 8, 8); nvgFill(a.vg);
        for (int i = 0; i < 12; ++i) {
            nvgBeginPath(a.vg); nvgRect(a.vg, 10 + i * 40, 40, 32, 140);
            nvgFillColor(a.vg, n & (1 << i) ? nvgRGB(255, 255, 255) : nvgRGB(16, 16, 16)); nvgFill(a.vg);
        }
        auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        if (font) {
            nvgFontFaceId(a.vg, font->handle); nvgFontSize(a.vg, 20); nvgFillColor(a.vg, nvgRGB(220, 230, 240));
            nvgText(a.vg, 14, 220, string::f("SYNC CHECK     frame code %04d", n).c_str(), nullptr);
            nvgFontSize(a.vg, 12); nvgText(a.vg, 14, 246, "CV channel 1 = displayed integer code. Channels 2–13 = barcode bits.", nullptr);
        }
    }
};

struct SyncTestWidget : app::ModuleWidget {
    SyncTestWidget(SyncTest* module) {
        setModule(module); box.size = Vec(540, 380);
        auto* display = new TestDisplay; display->module = module; display->box = Rect(Vec(20, 20), Vec(500, 270)); addChild(display);
        addParam(createParamCentered<LEDButton>(Vec(80, 330), module, SyncTest::RUN));
        addInput(createInputCentered<PJ301MPort>(Vec(190, 330), module, SyncTest::READY));
        addOutput(createOutputCentered<PJ301MPort>(Vec(310, 330), module, SyncTest::GATE));
        addOutput(createOutputCentered<PJ301MPort>(Vec(430, 330), module, SyncTest::POLY));
    }
    void draw(const DrawArgs& a) override {
        nvgBeginPath(a.vg); nvgRect(a.vg, 0, 0, box.size.x, box.size.y); nvgFillColor(a.vg, nvgRGB(17, 24, 35)); nvgFill(a.vg);
        ModuleWidget::draw(a);
        auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        if (!font) return;
        nvgFontFaceId(a.vg, font->handle); nvgFontSize(a.vg, 11); nvgTextAlign(a.vg, NVG_ALIGN_CENTER);
        nvgFillColor(a.vg, nvgRGB(210, 220, 230));
        const char* labels[] = {"RUN ONCE", "READY IN", "GATE OUT", "POLY CV"};
        for (int i = 0; i < 4; ++i) nvgText(a.vg, 80 + i * 116.66f, 360, labels[i], nullptr);
    }
    void appendContextMenu(ui::Menu* menu) override {
        auto* m = dynamic_cast<SyncTest*>(module); if (!m) return;
        menu->addChild(new ui::MenuSeparator);
        menu->addChild(createMenuLabel("Connect READY, GATE and POLY to FrameSync Recorder."));
        for (int seconds : {5, 10, 30, 60}) menu->addChild(createCheckMenuItem(string::f("%d-second check", seconds), "", [m, seconds] { return m->duration == seconds; }, [m, seconds] { m->duration = seconds; }));
    }
};

Model* modelSyncTest = createModel<SyncTest, SyncTestWidget>("SyncTest");
