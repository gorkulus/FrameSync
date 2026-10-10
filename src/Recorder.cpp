// Derived in part from VCV Recorder's framebuffer capture pattern.
// Copyright © 2019–2020 Andrew Belt (upstream portions). GPL-3.0-or-later.
#include "RecorderModule.hpp"
#include <functional>
#include <cstring>

static void textAt(NVGcontext* vg, float x, float y, float size, const std::string& text, NVGcolor color = nvgRGB(208, 219, 231), int align = NVG_ALIGN_LEFT) {
    auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
    if (!font) return;
    nvgFontFaceId(vg, font->handle); nvgFontSize(vg, size);
    nvgTextAlign(vg, align | NVG_ALIGN_MIDDLE); nvgFillColor(vg, color); nvgText(vg, x, y, text.c_str(), nullptr);
}

struct ActionButton : widget::OpaqueWidget {
    std::string label;
    std::function<void()> action;
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg); nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3);
        nvgFillColor(args.vg, nvgRGB(37, 52, 69)); nvgFill(args.vg);
        textAt(args.vg, box.size.x / 2, box.size.y / 2, 10, label, nvgRGB(227, 236, 245), NVG_ALIGN_CENTER);
    }
    void onButton(const event::Button& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) { e.consume(this); if (action) action(); }
        else OpaqueWidget::onButton(e);
    }
};

struct RegionPicker : widget::OpaqueWidget {
    int64_t moduleId;
    int x = 0, y = 0, size = 512, width = 0, height = 0;
    float sx = 1.f, sy = 1.f;
    bool followsMouse = true;
    explicit RegionPicker(FrameSync* module) : moduleId(module->id), x(module->config.x), y(module->config.y), size(module->config.size) { module->selecting = true; }
    FrameSync* target() { return dynamic_cast<FrameSync*>(APP->engine->getModule(moduleId)); }
    ~RegionPicker() override { if (auto* m = target()) m->selecting = false; }
    void step() override {
        if (!target()) { requestDelete(); return; }
        box.size = APP->scene->box.size;
        glfwGetFramebufferSize(APP->window->win, &width, &height);
        sx = width / std::max(box.size.x, 1.f); sy = height / std::max(box.size.y, 1.f);
        x = std::max(0, std::min(x, width - size)); y = std::max(0, std::min(y, height - size));
        OpaqueWidget::step();
    }
    void draw(const DrawArgs& a) override {
        float px = x / sx, py = y / sy, w = size / sx, h = size / sy;
        nvgBeginPath(a.vg); nvgRect(a.vg, 0, 0, box.size.x, box.size.y);
        nvgRect(a.vg, px, py, w, h); nvgPathWinding(a.vg, NVG_HOLE);
        nvgFillColor(a.vg, nvgRGBA(4, 9, 17, 170)); nvgFill(a.vg);
        nvgBeginPath(a.vg); nvgRect(a.vg, px - 1, py - 1, w + 2, h + 2);
        nvgStrokeWidth(a.vg, 2); nvgStrokeColor(a.vg, nvgRGB(54, 230, 199)); nvgStroke(a.vg);
        nvgBeginPath(a.vg); nvgRect(a.vg, 10, 10, std::min(680.f, box.size.x - 20), 60); nvgFillColor(a.vg, nvgRGB(14, 22, 33)); nvgFill(a.vg);
        textAt(a.vg, 24, 30, 15, string::f("%d × %d pixels — move to position, click to select", size, size));
        textAt(a.vg, 24, 53, 12, width < size || height < size ? "Enlarge the Rack window to fit this preset. Esc cancels." : "Arrows: 1 pixel · Shift+arrows: 10 pixels · Enter: select · Esc: cancel");
    }
    void commit() {
        if (width < size || height < size) return;
        if (auto* m = target()) { m->config.x = x; m->config.y = y; m->regionSet = true; }
        requestDelete();
    }
    void onHover(const event::Hover& e) override {
        if (followsMouse) { x = std::max(0, std::min(int(e.pos.x * sx) - size / 2, width - size)); y = std::max(0, std::min(int(e.pos.y * sy) - size / 2, height - size)); }
        e.consume(this);
    }
    void onButton(const event::Button& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) commit();
        e.consume(this);
    }
    void onHoverKey(const event::HoverKey& e) override {
        if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
            int d = e.mods & GLFW_MOD_SHIFT ? 10 : 1;
            if (e.key == GLFW_KEY_ESCAPE) requestDelete();
            else if (e.key == GLFW_KEY_ENTER) commit();
            else if (e.key == GLFW_KEY_LEFT) { x -= d; followsMouse = false; }
            else if (e.key == GLFW_KEY_RIGHT) { x += d; followsMouse = false; }
            else if (e.key == GLFW_KEY_UP) { y -= d; followsMouse = false; }
            else if (e.key == GLFW_KEY_DOWN) { y += d; followsMouse = false; }
        }
        e.consume(this);
    }
};

struct RecorderPanel : widget::Widget {
    FrameSync* module = nullptr;
    void draw(const DrawArgs& a) override {
        nvgBeginPath(a.vg); nvgRect(a.vg, 0, 0, box.size.x, box.size.y); nvgFillColor(a.vg, nvgRGB(17, 24, 35)); nvgFill(a.vg);
        nvgBeginPath(a.vg); nvgRect(a.vg, 0, 0, box.size.x, 3); nvgFillColor(a.vg, nvgRGB(54, 230, 199)); nvgFill(a.vg);
        textAt(a.vg, 15, 23, 17, "FRAME / SYNC");
        std::string timing = "60 FPS";
        textAt(a.vg, 255, 24, 10, timing, nvgRGB(54, 230, 199), NVG_ALIGN_RIGHT);
        nvgBeginPath(a.vg); nvgRoundedRect(a.vg, 14, 40, 242, 42, 4); nvgFillColor(a.vg, nvgRGB(7, 13, 21)); nvgFill(a.vg);
        State s = module ? module->capture.state() : State::Idle;
        const char* states[] = {"DISARMED", "PREPARING", "PRIMING", "READY", "RECORDING", "SAVING", "ERROR"};
        std::string stateText = states[int(s)];
        if (module && module->config.cvDelayFrames) stateText += string::f(" / CSV +%df", module->config.cvDelayFrames);
        textAt(a.vg, 24, 54, 12, stateText, s == State::Error ? nvgRGB(255, 112, 112) : nvgRGB(54, 230, 199));
        std::string detail = module ? string::f("%d²   %llu frames   %llu repeats", module->config.size, (unsigned long long)module->capture.frameCount(), (unsigned long long)module->capture.repeats()) : "512²   VIDEO + AUDIO + CSV";
        textAt(a.vg, 24, 71, 10, detail);
        const char* controls[] = {"GATE", "START", "STOP", "TOGGLE", "POLY CV"};
        for (int i = 0; i < 5; ++i) textAt(a.vg, 27 + i * 54, 125, 9, controls[i], nvgRGB(160, 180, 200), NVG_ALIGN_CENTER);
        for (int i = 0; i < 16; ++i) {
            std::string name = module ? module->config.names[i] : string::f("cv%02d", i + 1);
            if (name.size() > 9) name = name.substr(0, 8) + "…";
            textAt(a.vg, 36 + (i % 4) * 66, 169 + (i / 4) * 42, 9, name, nvgRGB(160, 180, 200), NVG_ALIGN_CENTER);
        }
        const char* bottom[] = {"L/MONO", "R", "READY", "REC", "SAVED"};
        for (int i = 0; i < 5; ++i) textAt(a.vg, 27 + i * 54, 340, 9, bottom[i], i < 2 ? nvgRGB(160, 180, 200) : nvgRGB(54, 230, 199), NVG_ALIGN_CENTER);
    }
};

struct NameField : ui::TextField {
    std::function<void(const std::string&)> save;
    void onChange(const event::Change& e) override { if (save) save(text); TextField::onChange(e); }
};

struct FrameSyncWidget : app::ModuleWidget {
    std::vector<uint8_t> bottomUp, topDown;
    int warmup = 3;
    ActionButton* armButton = nullptr;
#ifdef FRAMESYNC_TEST_HARNESS
    double testOpened = 0;
    bool testStarted = false;
#endif
    FrameSyncWidget(FrameSync* module) {
        setModule(module); box.size = Vec(270, 380);
        auto* panel = new RecorderPanel; panel->module = module; panel->box.size = box.size; addChild(panel);
        auto button = [&](float x, const std::string& label, std::function<void()> action) {
            auto* b = new ActionButton; b->box = Rect(Vec(x, 91), Vec(55, 23)); b->label = label; b->action = action; addChild(b); return b;
        };
        armButton = button(14, "ARM", [module] { if (module) module->arm(); });
        button(76, "REC", [module] { if (module) module->command = 3; });
        button(138, "AREA", [this] { selectRegion(); });
        button(200, "SIZE", [module] { if (module && !module->active()) { module->config.size = module->config.size == 512 ? 1024 : 512; module->regionSet = false; } });
        const int controls[] = {FrameSync::GATE, FrameSync::START, FrameSync::STOP, FrameSync::TOGGLE, FrameSync::POLY};
        for (int i = 0; i < 5; ++i) addInput(createInputCentered<PJ301MPort>(Vec(27 + 54 * i, 144), module, controls[i]));
        for (int i = 0; i < 16; ++i) addInput(createInputCentered<PJ301MPort>(Vec(36 + (i % 4) * 66, 188 + (i / 4) * 42), module, FrameSync::CV + i));
        addInput(createInputCentered<PJ301MPort>(Vec(27, 359), module, FrameSync::AUDIO_L));
        addInput(createInputCentered<PJ301MPort>(Vec(81, 359), module, FrameSync::AUDIO_R));
        for (int i = 0; i < 3; ++i) addOutput(createOutputCentered<PJ301MPort>(Vec(135 + i * 54, 359), module, i));
    }
    void selectRegion() {
        auto* m = dynamic_cast<FrameSync*>(module);
        if (!m || m->selecting || m->capture.state() == State::Recording || m->capture.state() == State::Finishing) return;
        m->capture.disarm(); warmup = 3;
        APP->scene->addChild(new RegionPicker(m));
    }
    void step() override {
        ModuleWidget::step();
        auto* m = dynamic_cast<FrameSync*>(module); if (!m) return;
#ifdef FRAMESYNC_TEST_HARNESS
        // Only present in the isolated integration-test build.
        if (m->testArmOnLoad) {
            if (testOpened == 0) testOpened = glfwGetTime();
            if (!testStarted && glfwGetTime() - testOpened > 3) {
                if (auto* target = APP->scene->rack->getModule(2)) {
                    int fw, fh; glfwGetFramebufferSize(APP->window->win, &fw, &fh);
                    Vec p = target->getAbsoluteOffset(Vec(20, 20));
                    m->config.x = int(std::lround(p.x * fw / APP->scene->box.size.x));
                    m->config.y = int(std::lround(p.y * fh / APP->scene->box.size.y));
                    m->regionSet = true; m->arm(); testStarted = true;
                }
            }
            if (m->capture.completed() || m->capture.state() == State::Error || glfwGetTime() - testOpened > 90) {
                INFO("FrameSync integration test: %s", m->capture.message().c_str());
                m->capture.disarm(); glfwSetWindowShouldClose(APP->window->win, GLFW_TRUE);
            }
        }
#endif
        armButton->label = m->active() ? "DISARM" : "ARM";
        if (m->rateError.exchange(false)) m->capture.fail("Engine sample rate changed; re-arm before the next take");
        if (!m->capture.wantsPixels() || m->selecting) { warmup = 3; return; }
        if (warmup > 0) { --warmup; return; }
        int width, height; glfwGetFramebufferSize(APP->window->win, &width, &height);
        int size = m->config.size, x = m->config.x, y = m->config.y;
        if (x < 0 || y < 0 || x + size > width || y + size > height) { m->capture.fail("Capture area no longer fits the Rack window; select it again"); return; }
        size_t bytes = size_t(size) * size * 4;
        bottomUp.resize(bytes); topDown.resize(bytes);
        GLint oldRead, oldBuffer, oldPack, oldPbo, oldRow, oldSkipRows, oldSkipPixels;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glGetIntegerv(GL_READ_BUFFER, &oldBuffer); glGetIntegerv(GL_PACK_ALIGNMENT, &oldPack);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPbo); glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &oldRow); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &oldSkipRows); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &oldSkipPixels); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadBuffer(GL_FRONT); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, height - y - size, size, size, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
        glPixelStorei(GL_PACK_ALIGNMENT, oldPack); glReadBuffer(oldBuffer); glBindFramebuffer(GL_READ_FRAMEBUFFER, oldRead);
        glPixelStorei(GL_PACK_ROW_LENGTH, oldRow); glPixelStorei(GL_PACK_SKIP_ROWS, oldSkipRows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, oldSkipPixels); glBindBuffer(GL_PIXEL_PACK_BUFFER, oldPbo);
        for (int row = 0; row < size; ++row)
            std::memcpy(topDown.data() + size_t(row) * size * 4, bottomUp.data() + size_t(size - row - 1) * size * 4, size * 4);
        m->capture.submitPixels(topDown.data(), m->latestSample.load(std::memory_order_relaxed));
    }
    void appendContextMenu(ui::Menu* menu) override {
        auto* m = dynamic_cast<FrameSync*>(module); if (!m) return;
        menu->addChild(new ui::MenuSeparator);
        menu->addChild(createMenuLabel(m->capture.message()));
        if (m->config.directory.empty()) menu->addChild(createMenuLabel("Output folder not selected"));
        else menu->addChild(createMenuLabel(m->config.directory));
        menu->addChild(createMenuItem("Choose output folder…", "", [m] { m->chooseFolder(); }, m->active()));
        menu->addChild(createMenuItem("Select capture area…", "", [this] { selectRegion(); }, m->capture.state() == State::Recording));
        for (int size : {512, 1024}) menu->addChild(createCheckMenuItem(string::f("%d × %d pixels", size, size), "", [m, size] { return m->config.size == size; }, [m, size] { if (!m->active()) { m->config.size = size; m->regionSet = false; } }, m->active()));
        menu->addChild(createSubmenuItem("CSV delay compensation", m->config.cvDelayFrames ? string::f("%d frames", m->config.cvDelayFrames) : "Off", [m](ui::Menu* child) {
            child->addChild(createMenuLabel("Delay CV values to match audio/video"));
            for (int frames = 0; frames <= 4; ++frames) {
                std::string label = frames ? string::f("%d frame%s (%.1f ms)", frames, frames == 1 ? "" : "s", frames * 1000. / 60) : "Off (original timing)";
                child->addChild(createCheckMenuItem(label, "", [m, frames] { return m->config.cvDelayFrames == frames; }, [m, frames] { if (!m->active()) m->config.cvDelayFrames = frames; }, m->active()));
            }
            child->addChild(createMenuLabel("Disarm to change; wait for SAVED after STOP"));
        }, m->active()));
        menu->addChild(createSubmenuItem("Take prefix", m->config.prefix, [m](ui::Menu* child) {
            auto* f = new NameField; f->box.size.x = 210; f->setText(m->config.prefix);
            f->save = [m](const std::string& s) { if (!m->active()) m->config.prefix = s; }; child->addChild(f);
        }, m->active()));
        menu->addChild(createSubmenuItem("CSV column names", "", [m](ui::Menu* child) {
            for (int i = 0; i < 16; ++i) {
                child->addChild(createMenuLabel(string::f("Channel %02d", i + 1)));
                auto* f = new NameField; f->box.size.x = 210; f->setText(m->config.names[i]);
                f->save = [m, i](const std::string& s) { if (!m->active()) m->config.names[i] = s; }; child->addChild(f);
            }
        }, m->active()));
        if (!m->capture.lastTake().empty()) menu->addChild(createMenuLabel("Saved: " + m->capture.lastTake()));
        menu->addChild(createMenuLabel("60 fps · H.264 MP4 · one CSV row per frame"));
    }
};

Model* modelFrameSync = createModel<FrameSync, FrameSyncWidget>("Recorder");
