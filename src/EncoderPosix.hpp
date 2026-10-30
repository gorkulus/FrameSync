// GPL-3.0-or-later. Linux backend retained from FrameSync 2.1.0.
#pragma once
namespace framesync {
using Clock = std::chrono::steady_clock;
// Isolate FFmpeg in its own process. Stock VCV Recorder exports an older set of
// libav symbols; linking a newer system libav inside the same Rack process can
// interpose those symbols. No shell is used, and encoding happens during capture.
class VideoEncoder {
    pid_t pid_ = -1;
    int input_ = -1;
public:
    ~VideoEncoder() { cancel(); }
    void open(const Config& c, const std::string& path, const std::string& log) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC)) throw std::runtime_error("Cannot create encoder pipe");
        int logFd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (logFd < 0) { ::close(fds[0]); ::close(fds[1]); throw std::runtime_error("Cannot create encoder log"); }
        auto arguments = videoArguments(c, path);
        std::vector<char*> argv;
        for (auto& a : arguments) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, logFd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, logFd, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
        int result = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[0]); ::close(logFd);
        if (result) { ::close(fds[1]); pid_ = -1; throw std::runtime_error("Cannot start FFmpeg: " + std::string(strerror(result))); }
        input_ = fds[1];
        fcntl(input_, F_SETFL, fcntl(input_, F_GETFL) | O_NONBLOCK);
    }
    void write(const std::vector<uint8_t>& bytes, const std::atomic<bool>& quitting) {
        size_t offset = 0;
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (offset < bytes.size()) {
            ssize_t n = ::write(input_, bytes.data() + offset, bytes.size() - offset);
            if (n > 0) { offset += size_t(n); continue; }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw std::runtime_error("Video encoder stopped; see the take's encoder log");
            if (Clock::now() > deadline || quitting.load()) throw std::runtime_error("Video encoder timed out or capture was interrupted");
            pollfd p{input_, POLLOUT, 0}; poll(&p, 1, 20);
        }
    }
    void finish(const std::atomic<bool>& quitting, double timeout = 15) {
        if (input_ >= 0) { ::close(input_); input_ = -1; }
        auto deadline = Clock::now() + std::chrono::milliseconds(int64_t(timeout * 1000));
        while (pid_ > 0) {
            int status = 0;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                pid_ = -1;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("FFmpeg could not finalize the MP4; see encoder log");
                return;
            }
            if (r < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for encoder");
            if (quitting.load()) throw std::runtime_error("Encoding interrupted; partial take preserved");
            if (Clock::now() > deadline) throw std::runtime_error("Encoder finalization timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    void muxAudio(const Config& c, const std::string& video, const std::string& audio,
                  const std::string& output, const std::string& log, int64_t frames,
                  const std::atomic<bool>& quitting) {
        // Video is copied losslessly. Encode the exact engine-sample audio, and
        // pad only the fraction of the last video frame after the recording gate.
        // Both streams start at zero; CSV correction never changes either stream.
        auto arguments = audioArguments(c, video, audio, output, frames);
        std::vector<char*> argv;
        for (auto& a : arguments) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        int fd = ::open(log.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("Cannot open audio encoder log");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
        int result = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions); ::close(fd);
        if (result) { pid_ = -1; throw std::runtime_error("Cannot start audio encoder: " + std::string(strerror(result))); }
        finish(quitting, 30. + double(frames) / c.fps / 2.);
    }
    void cancel() {
        if (input_ >= 0) { ::close(input_); input_ = -1; }
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
    }
};

}
