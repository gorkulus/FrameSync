// Native Windows subprocess backend. GPL-3.0-or-later.
#pragma once
namespace framesync {
namespace winEncoder {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
        value = h;
    }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
inline std::runtime_error error(const std::string& message) {
    return std::runtime_error(message + " (Windows error " + std::to_string(GetLastError()) + ")");
}
// Quote every argument using the Windows CRT argv rules, including trailing
// backslashes and embedded quotes. No command interpreter is involved.
inline std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result += ch; slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    return result + L'"';
}
inline std::wstring executable(const std::string& name) {
    std::wstring wide = utf8Path(name).wstring();
    if (wide.find_first_of(L"/\\:") != std::wstring::npos) return wide;
    DWORD count = SearchPathW(nullptr, wide.c_str(), nullptr, 0, nullptr, nullptr);
    if (!count) throw error("FFmpeg not found: choose ffmpeg.exe in the module menu or add it to PATH");
    std::vector<wchar_t> buffer(count + 1);
    if (!SearchPathW(nullptr, wide.c_str(), nullptr, DWORD(buffer.size()), buffer.data(), nullptr))
        throw error("Cannot resolve FFmpeg executable");
    return buffer.data();
}
}

class VideoEncoder {
    winEncoder::Handle process_, input_;
    using Clock = std::chrono::steady_clock;
    void launch(const std::vector<std::string>& arguments, HANDLE input, HANDLE log) {
        using namespace winEncoder;
        std::wstring exe = executable(arguments.front());
        std::wstring command = quote(exe);
        for (size_t i = 1; i < arguments.size(); ++i) command += L" " + quote(utf8Path(arguments[i]).wstring());
        std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(0);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> storage(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))
            throw error("Cannot prepare FFmpeg handle inheritance");
        HANDLE handles[] = {input, log};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      handles, sizeof(handles), nullptr, nullptr)) {
            auto failure = error("Cannot restrict FFmpeg handle inheritance");
            DeleteProcThreadAttributeList(startup.lpAttributeList); throw failure;
        }
        PROCESS_INFORMATION child{};
        BOOL ok = CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
        DWORD lastError = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!ok) { SetLastError(lastError); throw error("Cannot start FFmpeg; check the executable setting"); }
        CloseHandle(child.hThread); process_.reset(child.hProcess);
    }
public:
    ~VideoEncoder() { cancel(); }
    void open(const Config& c, const std::string& path, const std::string& log) {
        using namespace winEncoder;
        static std::atomic<uint64_t> sequence{0};
        std::wstring name = L"\\\\.\\pipe\\FrameSync-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(++sequence);
        // An overlapped named pipe lets the worker interrupt a blocked write.
        input_.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr));
        if (!input_.valid()) throw error("Cannot create FFmpeg input pipe");
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle reader(CreateFileW(name.c_str(), GENERIC_READ, 0, &inheritable, OPEN_EXISTING, 0, nullptr));
        if (!reader.valid()) throw error("Cannot open FFmpeg input pipe");
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) throw error("Cannot create FFmpeg pipe event");
        OVERLAPPED connection{}; connection.hEvent = event.value;
        if (!ConnectNamedPipe(input_.value, &connection)) {
            DWORD code = GetLastError();
            if (code != ERROR_PIPE_CONNECTED) throw error("Cannot connect FFmpeg pipe");
        }
        Handle logFile(CreateFileW(utf8Path(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!logFile.valid()) throw error("Cannot create encoder log");
        launch(videoArguments(c, path), reader.value, logFile.value);
    }
    void write(const std::vector<uint8_t>& bytes, const std::atomic<bool>& quitting) {
        using namespace winEncoder;
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) throw error("Cannot create encoder write event");
        auto deadline = Clock::now() + std::chrono::seconds(10);
        size_t offset = 0;
        while (offset < bytes.size()) {
            if (quitting.load() || Clock::now() > deadline)
                throw std::runtime_error("Video encoder timed out or capture was interrupted");
            OVERLAPPED operation{}; operation.hEvent = event.value; ResetEvent(event.value);
            DWORD written = 0;
            BOOL ok = WriteFile(input_.value, bytes.data() + offset,
                DWORD(std::min<size_t>(65536, bytes.size() - offset)), &written, &operation);
            if (!ok && GetLastError() != ERROR_IO_PENDING)
                throw error("Video encoder stopped; see the take's encoder log");
            if (!ok) {
                while (WaitForSingleObject(event.value, 20) == WAIT_TIMEOUT) {
                    if (quitting.load() || Clock::now() > deadline) {
                        CancelIoEx(input_.value, &operation);
                        // Reap cancellation before destroying OVERLAPPED/buffer.
                        GetOverlappedResult(input_.value, &operation, &written, TRUE);
                        throw std::runtime_error("Video encoder timed out or capture was interrupted");
                    }
                }
                if (!GetOverlappedResult(input_.value, &operation, &written, FALSE))
                    throw error("Video encoder stopped; see the take's encoder log");
            }
            if (!written) throw std::runtime_error("FFmpeg pipe made no progress");
            offset += written;
        }
    }
    void finish(const std::atomic<bool>& quitting, double timeout = 15) {
        input_.reset();
        auto deadline = Clock::now() + std::chrono::milliseconds(int64_t(timeout * 1000));
        while (process_.valid()) {
            DWORD result = WaitForSingleObject(process_.value, 20);
            if (result == WAIT_OBJECT_0) {
                DWORD status = 0;
                if (!GetExitCodeProcess(process_.value, &status)) throw winEncoder::error("Cannot query FFmpeg exit status");
                process_.reset();
                if (status != 0) throw std::runtime_error("FFmpeg could not finalize the MP4; see encoder log");
                return;
            }
            if (result == WAIT_FAILED) throw winEncoder::error("Cannot wait for FFmpeg");
            if (quitting.load()) throw std::runtime_error("Encoding interrupted; partial take preserved");
            if (Clock::now() > deadline) throw std::runtime_error("Encoder finalization timed out");
        }
    }
    void muxAudio(const Config& c, const std::string& video, const std::string& audio,
            const std::string& output, const std::string& log, int64_t frames, const std::atomic<bool>& quitting) {
        using namespace winEncoder;
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inheritable, OPEN_EXISTING, 0, nullptr));
        Handle logFile(CreateFileW(utf8Path(log).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
            &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!nullInput.valid() || !logFile.valid()) throw error("Cannot open audio encoder log/input");
        launch(audioArguments(c, video, audio, output, frames), nullInput.value, logFile.value);
        finish(quitting, 30. + double(frames) / c.fps / 2.);
    }
    void cancel() {
        input_.reset();
        if (process_.valid()) {
            // This HANDLE belongs only to the encoder child created by launch().
            if (WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) TerminateProcess(process_.value, 1);
            WaitForSingleObject(process_.value, 2000); process_.reset();
        }
    }
};
}
