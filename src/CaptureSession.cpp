// FrameSync: GPL-3.0-or-later. See LICENSE.md for upstream attribution.
#include "CaptureSession.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>
#include <stdexcept>
#include <cerrno>
#include "Encoder.hpp"

namespace framesync {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

Config::Config() {
    for (int i = 0; i < CHANNELS; ++i) {
        std::ostringstream s; s << "cv" << std::setw(2) << std::setfill('0') << i + 1;
        names[i] = s.str();
    }
}

static std::string csvQuote(const std::string& value) {
    std::string s = "\"";
    for (char c : value) { s += c; if (c == '"') s += '"'; }
    return s + '"';
}
static std::string jsonQuote(const std::string& value) {
    std::ostringstream s; s << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') s << '\\' << c;
        else if (c < 32) s << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else s << c;
    }
    return s.str() + '"';
}

CaptureSession::CaptureSession() : worker_([this] { run(); }) {}
CaptureSession::~CaptureSession() { shutdown(); }
void CaptureSession::shutdown() {
    keepArmed_ = false; quit_ = true; wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
std::string CaptureSession::message() const { std::lock_guard<std::mutex> l(textMutex_); return message_; }
std::string CaptureSession::lastTake() const { std::lock_guard<std::mutex> l(textMutex_); return lastTake_; }
void CaptureSession::setMessage(const std::string& s) { std::lock_guard<std::mutex> l(textMutex_); message_ = s; }
bool CaptureSession::arm(const Config& c) {
    State s = state();
    if (s != State::Idle && s != State::Error) return false;
    if (c.directory.empty() || (c.size != 512 && c.size != 1024) || c.fps != 60 || c.sampleRate < 8000 || c.sampleRate > 768000
        || c.cvDelayFrames < 0 || c.cvDelayFrames > 4) {
        setMessage("Choose an output folder and valid capture settings"); return false;
    }
    std::set<std::string> names{"frame", "time_seconds", "sample_index", "capture_sample_index", "capture_offset_ms", "repeated_image", "cv_sample_index", "cv_time_seconds", "cv_history_clamped"};
    for (const auto& name : c.names) {
        if (name.empty() || !names.insert(name).second) {
            setMessage("CSV names must be nonempty, unique, and different from timing columns"); return false;
        }
    }
    { std::lock_guard<std::mutex> l(mutex_); config_ = c; }
    keepArmed_ = true; overflow_ = false; audioOverflow_ = false; failed_ = false;
    state_.store(State::Preparing, std::memory_order_release); wake_.notify_one(); return true;
}
void CaptureSession::disarm() { keepArmed_ = false; wake_.notify_one(); }
bool CaptureSession::push(const Event& event) {
    auto* e = events_.reserve();
    if (!e) { overflow_ = true; return false; }
    *e = event; e->generation = generation_.load(); events_.commit(); return true;
}
bool CaptureSession::begin(int64_t sample) {
    if (!keepArmed_.load()) return false;
    State ready = State::Ready;
    if (!state_.compare_exchange_strong(ready, State::Recording)) return false;
    audioPartial_.count = 0;
    Event e; e.type = Event::Start; e.sample = sample; return push(e);
}
bool CaptureSession::frame(int64_t index, int64_t sample, const std::array<float, CHANNELS>& cv, int64_t cvSample, bool cvClamped) {
    if (state() != State::Recording) return false;
    Event e; e.type = Event::Frame; e.index = index; e.sample = sample; e.cv = cv;
    e.cvSample = cvSample == INT64_MIN ? sample : cvSample; e.cvClamped = cvClamped; return push(e);
}
bool CaptureSession::flushAudio() {
    if (!audioPartial_.count) return true;
    auto* block = audioBlocks_->reserve();
    if (!block) { audioOverflow_ = true; audioPartial_.count = 0; return false; }
    *block = audioPartial_; block->generation = generation_.load();
    audioBlocks_->commit(); audioPartial_.count = 0; return true;
}
bool CaptureSession::audio(int64_t sample, float left, float right) {
    if (state() != State::Recording) return false;
    if (!audioPartial_.count) audioPartial_.firstSample = sample;
    if (sample != audioPartial_.firstSample + audioPartial_.count) { audioOverflow_ = true; return false; }
    audioPartial_.samples[2 * audioPartial_.count] = left;
    audioPartial_.samples[2 * audioPartial_.count + 1] = right;
    ++audioPartial_.count;
    return audioPartial_.count < AUDIO_BLOCK_SAMPLES || flushAudio();
}
void CaptureSession::end(int64_t sample) {
    if (state() != State::Recording) return;
    flushAudio();
    Event e; e.type = Event::Stop; e.sample = sample;
    state_.store(State::Finishing); push(e);
}
bool CaptureSession::wantsPixels() const {
    State s = state(); return s == State::Priming || s == State::Ready || s == State::Recording;
}
bool CaptureSession::submitPixels(const uint8_t* rgba, int64_t sample) {
    if (!wantsPixels()) return false;
    auto* p = pixels_.reserve();
    if (!p) { missedImages_++; return false; }
    p->generation = generation_.load();
    p->rgba.resize(pixelBytes_.load());
    std::memcpy(p->rgba.data(), rgba, p->rgba.size());
    p->sample = sample; p->serial = ++imageSerial_; pixels_.commit(); return true;
}
void CaptureSession::fail(const std::string& reason) { setMessage(reason); failed_ = true; wake_.notify_one(); }

void CaptureSession::run() {
    // A failed encoder must never deliver SIGPIPE to Rack's process.
#ifndef _WIN32
    sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
    while (!quit_) {
        if (state() != State::Preparing) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(10)); continue;
        }
        Config c; { std::lock_guard<std::mutex> l(mutex_); c = config_; }
        uint64_t gen = ++generation_;
        pixelBytes_ = size_t(c.size) * c.size * 4;
        encoded_ = 0; repeated_ = 0; missedImages_ = 0;
        std::string base, error;
        bool started = false, stopped = false, successful = false;
        int64_t origin = 0, stopSample = 0;
        uint64_t lastSerial = 0, maxImageAge = 0;
        int64_t audioSamples = 0;
        uint64_t clippedAudio = 0, clampedCv = 0;
        VideoEncoder encoder;
        std::ofstream csv, audioFile;
        std::deque<Event> pending;
        std::deque<Pixels> history;
        auto lastImageAt = Clock::now();
        auto captureTarget = [&](const Event& e) {
            return e.sample;
        };
        auto emit = [&](const Event& e, const Pixels& image) {
            if (e.index != int64_t(encoded_.load())) throw std::runtime_error("Non-contiguous frame sequence; take stopped");
            if (e.sample - origin != frameSample(e.index, c.sampleRate, c.fps))
                throw std::runtime_error("CV sample boundary mismatch; take stopped");
            if ((!e.cvClamped && e.cvSample - origin != frameSample(e.index - c.cvDelayFrames, c.sampleRate, c.fps))
                || e.cvSample > e.sample)
                throw std::runtime_error("CSV source sample boundary mismatch; take stopped");
            if (e.cvClamped) ++clampedCv;
            for (float v : e.cv)
                if (!std::isfinite(v)) throw std::runtime_error("A CV input is not finite; take stopped");
            encoder.write(image.rgba, quit_);
            bool repeat = lastSerial == image.serial;
            lastSerial = image.serial;
            if (repeat) repeated_++;
            int64_t age = e.sample - image.sample;
            maxImageAge = std::max(maxImageAge, uint64_t(std::max<int64_t>(age, 0)));
            csv << e.index << ',' << std::setprecision(12) << double(e.index) / c.fps << ',' << e.sample - origin;
            for (float v : e.cv) {
                csv << ',' << std::setprecision(9) << v;
            }
            csv << ',' << image.sample - origin << ',' << std::setprecision(9) << double(image.sample - e.sample) * 1000. / c.sampleRate << ',' << (repeat ? 1 : 0)
                << ',' << e.cvSample - origin << ',' << std::setprecision(12) << double(e.cvSample - origin) / c.sampleRate << ',' << (e.cvClamped ? 1 : 0) << '\n';
            if (!csv) throw std::runtime_error("Cannot write CSV (disk full or write error)");
            encoded_++;
        };
        try {
            fs::create_directories(utf8Path(c.directory));
            std::string prefix;
            for (unsigned char ch : c.prefix) if (std::isalnum(ch) || ch == '_' || ch == '-') prefix += char(ch);
            if (prefix.empty()) prefix = "take";
            for (int i = 1; i < 1000000; ++i) {
                std::ostringstream s; s << prefix << '-' << std::setw(4) << std::setfill('0') << i;
                std::string candidate = (utf8Path(c.directory) / s.str()).u8string();
                if (fs::exists(utf8Path(candidate + ".mp4")) || fs::exists(utf8Path(candidate + ".csv")) || fs::exists(utf8Path(candidate + ".json")) || fs::exists(utf8Path(candidate + ".partial.mp4")) || fs::exists(utf8Path(candidate + ".partial.csv"))
                    || fs::exists(utf8Path(candidate + ".partial.video.mp4")) || fs::exists(utf8Path(candidate + ".partial.audio.f32"))) continue;
#ifdef _WIN32
                HANDLE reservation = CreateFileW(utf8Path(candidate + ".reserve").c_str(), GENERIC_WRITE,
                    0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (reservation != INVALID_HANDLE_VALUE) { CloseHandle(reservation); base = candidate; break; }
                if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                    throw std::runtime_error("Cannot reserve a take in the output folder");
#else
                int fd = ::open((candidate + ".reserve").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (fd >= 0) { ::close(fd); base = candidate; break; }
                if (errno != EEXIST) throw std::runtime_error("Cannot reserve a take in the output folder");
#endif
            }
            if (base.empty()) throw std::runtime_error("No free take number");
            csv.open(utf8Path(base + ".partial.csv"), std::ios::out | std::ios::trunc);
            csv.imbue(std::locale::classic());
            if (!csv) throw std::runtime_error("Cannot open CSV");
            csv << "frame,time_seconds,sample_index";
            for (const auto& name : c.names) csv << ',' << csvQuote(name);
            csv << ",capture_sample_index,capture_offset_ms,repeated_image,cv_sample_index,cv_time_seconds,cv_history_clamped\n";
            audioFile.open(utf8Path(base + ".partial.audio.f32"), std::ios::binary | std::ios::out | std::ios::trunc);
            if (!audioFile) throw std::runtime_error("Cannot open temporary audio file");
            encoder.open(c, base + ".partial.video.mp4", base + ".encoder.log");
            setMessage("Waiting for a clean capture frame");
            state_.store(State::Priming, std::memory_order_release);
            while (!quit_) {
                if (overflow_) throw std::runtime_error("CV queue overflow; take stopped without changing its timeline");
                if (audioOverflow_) throw std::runtime_error("Audio queue overflow or missing engine samples; take stopped without changing its timeline");
                if (failed_) throw std::runtime_error(message());
                while (Event* ptr = events_.front()) {
                    Event e = *ptr; events_.pop();
                    if (e.generation != gen) continue;
                    if (e.type == Event::Start) { started = true; origin = e.sample; setMessage("Recording"); }
                    else if (e.type == Event::Frame) pending.push_back(e);
                    else { stopped = true; stopSample = e.sample; }
                }
                while (auto* block = audioBlocks_->front()) {
                    if (block->generation != gen) { audioBlocks_->pop(); continue; }
                    // Start and audio use different queues. Wait for Start to
                    // establish the origin if the producer advanced mid-drain.
                    if (!started) break;
                    if (block->firstSample != origin + audioSamples)
                        throw std::runtime_error("Non-contiguous audio samples; take stopped");
                    for (int i = 0; i < block->count * 2; ++i) {
                        float& value = block->samples[i];
                        if (!std::isfinite(value)) throw std::runtime_error("An audio input is not finite; take stopped");
                        if (value < -1.f || value > 1.f) { ++clippedAudio; value = std::clamp(value, -1.f, 1.f); }
                    }
                    audioFile.write(reinterpret_cast<const char*>(block->samples.data()), block->count * 2 * sizeof(float));
                    if (!audioFile) throw std::runtime_error("Cannot write audio (disk full or write error)");
                    audioSamples += block->count; audioBlocks_->pop();
                }
                while (Pixels* next = pixels_.front()) {
                    if (next->generation != gen) { pixels_.pop(); continue; }
                    history.push_back(*next); pixels_.pop(); lastImageAt = Clock::now();
                    if (state() == State::Priming) {
                        setMessage("Ready — waiting for a recording gate"); state_.store(State::Ready);
                    }
                }
                bool flush = stopped || Clock::now() - lastImageAt > std::chrono::milliseconds(250);
                if (started && flush && history.empty()) throw std::runtime_error("No video frame available");
                // Retain a short history even while idle: the DSP and GUI queues
                // can be observed in either order. Select by timestamps, never by
                // whichever image happens to be current when the encoder wakes.
                while (!pending.empty() && !history.empty()
                       && (flush || history.back().sample >= captureTarget(pending.front()))) {
                    const Pixels* chosen = &history.front();
                    for (const auto& p : history) {
                        // Nearest read avoids an artificial whole-frame jump when
                        // the GUI lands a few samples after a scheduled boundary.
                        // One future capture provides the other side of the choice.
                        int64_t target = captureTarget(pending.front());
                        if (std::llabs(p.sample - target) < std::llabs(chosen->sample - target)) chosen = &p;
                        if (p.sample >= target) break;
                    }
                    emit(pending.front(), *chosen); pending.pop_front();
                }
                int64_t oldestNeeded = pending.empty() ? INT64_MAX : captureTarget(pending.front());
                while (history.size() > 8 && history[1].sample <= oldestNeeded) history.pop_front();
                // begin() publishes Recording before its Start event. A disarm
                // in that interval must not cancel the take before Start drains.
                State current = state();
                if ((stopped && pending.empty()) || (!keepArmed_ && !started
                    && current != State::Recording && current != State::Finishing)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (started && (!stopped || !pending.empty())) throw std::runtime_error("Recording interrupted before all frames were captured; partial take preserved");
            if (started && encoded_ > 0) {
                if (audioSamples != stopSample - origin) throw std::runtime_error("Audio sample count does not match recording interval; partial take preserved");
                encoder.finish(quit_); csv.flush(); audioFile.flush();
                if (!audioFile) throw std::runtime_error("Audio flush failed");
                if (!csv) throw std::runtime_error("CSV flush failed");
                csv.close(); audioFile.close();
                setMessage("Saving stereo audio + video");
                encoder.muxAudio(c, base + ".partial.video.mp4", base + ".partial.audio.f32", base + ".partial.mp4", base + ".encoder.log", encoded_.load(), quit_);
                fs::rename(utf8Path(base + ".partial.mp4"), utf8Path(base + ".mp4"));
                fs::rename(utf8Path(base + ".partial.csv"), utf8Path(base + ".csv"));
                fs::remove(utf8Path(base + ".partial.video.mp4")); fs::remove(utf8Path(base + ".partial.audio.f32"));
                successful = true;
            } else { csv.close(); audioFile.close(); encoder.cancel(); }
        } catch (const std::exception& e) {
            error = e.what(); state_.store(State::Finishing);
            encoder.cancel(); csv.close(); audioFile.close(); keepArmed_ = false;
        }
        if (!base.empty()) {
            try {
                if (started || !error.empty()) {
                    std::ofstream manifest(utf8Path(base + ".json")); manifest.imbue(std::locale::classic());
                    manifest << "{\n  \"status\": " << jsonQuote(successful ? "complete" : "error")
                        << ",\n  \"error\": " << jsonQuote(error)
                        << ",\n  \"fps\": " << c.fps << ",\n  \"sample_rate\": " << c.sampleRate
                        << ",\n  \"video_advance_frames\": 0"
                        << ",\n  \"cv_delay_frames\": " << c.cvDelayFrames
                        << ",\n  \"cv_delay_ms\": " << double(c.cvDelayFrames) * 1000. / c.fps
                        << ",\n  \"cv_history_clamped_rows\": " << clampedCv
                        << ",\n  \"audio_codec\": \"aac\",\n  \"audio_channels\": 2,\n  \"audio_sample_rate\": 48000"
                        << ",\n  \"audio_recorded_samples\": " << audioSamples
                        << ",\n  \"audio_clipped_channel_samples\": " << clippedAudio
                        << ",\n  \"audio_output_samples\": " << encoded_.load() * 800
                        << ",\n  \"width\": " << c.size << ",\n  \"height\": " << c.size
                        << ",\n  \"crop_x\": " << c.x << ",\n  \"crop_y\": " << c.y
                        << ",\n  \"frame_count\": " << encoded_.load()
                        << ",\n  \"recorded_engine_samples\": " << stopSample - origin
                        << ",\n  \"repeated_images\": " << repeated_.load()
                        << ",\n  \"capture_queue_overflows\": " << missedImages_.load()
                        << ",\n  \"max_capture_age_samples\": " << maxImageAge
                        << ",\n  \"cv_sampling\": \"frame onset minus CSV delay; cv_sample_index identifies the source engine sample\""
                        << ",\n  \"capture_timestamp_note\": \"timestamp at framebuffer read; not the scope internal render timestamp\"\n}\n";
                    manifest.close();
                    if (!manifest) { error = "Cannot write take metadata"; successful = false; keepArmed_ = false; }
                } else {
                    fs::remove(utf8Path(base + ".partial.csv")); fs::remove(utf8Path(base + ".partial.mp4")); fs::remove(utf8Path(base + ".partial.video.mp4")); fs::remove(utf8Path(base + ".partial.audio.f32")); fs::remove(utf8Path(base + ".encoder.log"));
                }
                fs::remove(utf8Path(base + ".reserve"));
            } catch (const std::exception& e) { error = e.what(); successful = false; keepArmed_ = false; }
        }
        if (successful) {
            { std::lock_guard<std::mutex> l(textMutex_); lastTake_ = base; }
            completed_++; setMessage("Saved " + utf8Path(base).filename().u8string());
        }
        if (!error.empty()) { setMessage(error); state_.store(State::Error); }
        else if (keepArmed_ && !quit_) state_.store(State::Preparing);
        else state_.store(State::Idle);
    }
}
}
