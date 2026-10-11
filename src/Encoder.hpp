// FFmpeg subprocess isolation, shared arguments, and UTF-8 filesystem paths.
// GPL-3.0-or-later.
#pragma once
#include "CaptureSession.hpp"
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace framesync {
inline std::filesystem::path utf8Path(const std::string& s) { return std::filesystem::u8path(s); }

inline std::string encoderExecutable(const Config& c) {
    if (!c.ffmpegPath.empty()) return c.ffmpegPath;
    if (const char* value = std::getenv("FRAMESYNC_FFMPEG")) if (*value) return value;
#ifdef _WIN32
    return "ffmpeg.exe";
#else
    return "/usr/bin/ffmpeg";
#endif
}
inline std::vector<std::string> videoArguments(const Config& c, const std::string& path) {
    return {encoderExecutable(c), "-hide_banner", "-loglevel", "warning", "-nostdin", "-n",
        "-f", "rawvideo", "-pixel_format", "rgba", "-video_size", std::to_string(c.size) + "x" + std::to_string(c.size),
        "-framerate", std::to_string(c.fps), "-i", "pipe:0", "-an", "-c:v", "libx264",
        "-preset", "veryfast", "-crf", std::to_string(c.crf), "-pix_fmt", "yuv420p",
        "-threads", "4", "-g", "1", "-bf", "0", "-movflags", "+faststart", "-f", "mp4", path};
}
inline std::vector<std::string> audioArguments(const Config& c, const std::string& video,
        const std::string& audio, const std::string& output, int64_t frames) {
    int64_t outputSamples = frames * (48000 / c.fps);
    return {encoderExecutable(c), "-hide_banner", "-loglevel", "warning", "-nostdin", "-n",
        "-i", video, "-f", "f32le", "-ar", std::to_string(c.sampleRate), "-ac", "2", "-i", audio,
        "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac", "-b:a", "320k",
        "-ar", "48000", "-ac", "2", "-af",
        "aresample=48000,apad=whole_len=" + std::to_string(outputSamples) + ",atrim=end_sample=" + std::to_string(outputSamples),
        "-movflags", "+faststart", "-f", "mp4", output};
}
}

#ifdef _WIN32
#include "EncoderWindows.hpp"
#else
#include "EncoderPosix.hpp"
#endif
