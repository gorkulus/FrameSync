# FrameSync: Windows port and setup handoff

Prepared 2026-10-09. The baseline is FrameSync 2.1.0, tested on Linux with Rack
Pro 2.6.6. **This source does not yet build or run on Windows.** No Windows
binary or Windows test result is included. Preserve the working Linux backend.

## User objective

Record a selected 512×512 or 1024×1024 region of a Rack oscilloscope at 60 fps,
with stereo audio embedded in the MP4 and one corresponding row of 16-channel
CV data per video frame. The files go directly into a separate ComfyUI workflow.
No manual cropping, audio merging, or CSV shifting should be needed.

The user is moving temporarily from Linux to Windows on a dual-boot machine.
Keep the project private; public release is only a future possibility. Port and
validate the recorder, then install it in Windows Rack. Do not change the user's
ComfyUI workflow or overwrite existing Rack patches/configuration.

## What to preserve

- Plugin slug `FrameSync`, recorder model slug `Recorder`, helper slug `SyncTest`.
- Existing input IDs: GATE 0, START 1, STOP 2, POLY CV 3, CV01–16 at 4–19,
  TOGGLE 20, AUDIO_L 21, AUDIO_R 22. Outputs READY 0, RECORDING 1, SAVED 2.
- TOGGLE starts on one rising edge, stops on the next. STOP wins simultaneous
  START/STOP or TOGGLE/STOP. Busy/disarmed edges are consumed, never queued.
- Audio uses every engine sample in [start, stop), scales ±10 V to full scale,
  sums polyphonic audio inputs, and copies L to R when R is unpatched.
- Video and audio have the same origin. Video runs at 60 fps; AAC output is stereo
  48 kHz / 320 kb/s. An incomplete final video frame is padded with audio silence.
- CSV-only compensation is Off or 1–4 frames. It samples CV at
  `ceil((frame - delay) * engineRate / 60)` relative to the recording origin.
  Audio/video never shift when changing this option. Keep pre-trigger CV history.
- `cvDelayFrames` is the current saved setting. Legacy `videoAdvanceFrames`
  migrates its numerical value to CSV-only delay. Do not revive old video advance.
- CSV frame IDs start at zero and match decoded video presentation order. Preserve
  headers, capture diagnostics, CV-source timestamps, and clamped-history flags.
- The module auto-rearms after saving. SAVED means the MP4, CSV, and metadata are
  finalized. A failed take preserves diagnostics rather than silently losing time.
- No file writes, encoding, allocation, or waiting in the engine process callback.

## Source map and actual Windows blockers

`src/RecorderModule.hpp` handles engine sampling, triggers, audio routing, CV
history, and saved settings. `src/Timeline.hpp` contains the rational frame
scheduler and fixed-size queues/history. These are mostly portable C++/Rack API.

`src/Recorder.cpp` handles the panel, region selector, and OpenGL framebuffer
reads, following VCV Recorder's capture approach. This should be portable but
must be checked on Windows, especially high-DPI scaling and GL_FRONT capture.
It captures Rack's framebuffer, not arbitrary desktop windows.

`src/CaptureSession.cpp` is the main porting work:

- `VideoEncoder` hardcodes `/usr/bin/ffmpeg` in both video encoding and audio mux.
- POSIX process/pipe code uses `pipe2`, `posix_spawn`, `fcntl`, `poll`, `waitpid`,
  `kill`, `close`, `/dev/null`, and Unix file descriptors.
- The worker blocks SIGPIPE via `pthread_sigmask`.
- Take reservation also uses POSIX `open(O_CREAT | O_EXCL | O_CLOEXEC)`.
- Narrow filesystem paths need an explicit UTF-8/Windows Unicode strategy.

The Makefile's module-level test link command assumes Linux `-lRack` and rpath;
use the Windows SDK's import library and runtime DLL lookup instead. Review
thread flags against the SDK/compiler. The core test has a Unix header that may
be removable; do not assume every test target already runs on Windows.

## Suggested implementation

1. Install native Windows Rack 2 x64 and the official Windows x64 Rack SDK.
   Using 2.6.6 initially matches the Linux baseline; if unavailable, document the
   Rack/SDK versions actually used. Free standalone should suffice for this
   plugin; use the user's Pro installation if preferred/available.
2. Use VCV's supported MSYS2 MinGW64 environment to build a native Windows DLL.
   Building the existing Linux target in WSL does not produce a Windows plugin.
   Keep build/source/SDK paths simple (e.g. C:\dev\FrameSync, no spaces).
3. Install a native Windows FFmpeg with libx264 and AAC, plus ffprobe for tests.
   For the first port keep FFmpeg external, as it is on Linux. Resolve an explicit
   executable path or a documented setting/PATH fallback and fail clearly if
   missing. Do not require a hardcoded machine-specific installation path.
4. Extract the encoder process handling into Linux and Windows implementations,
   preserving the Linux path. A Windows implementation can use CreateProcessW
   with redirected stdin/stdout/stderr and explicit handle inheritance. Handle
   Windows argument quoting and paths with spaces; invoke no command shell.
   Use a cancellable/bounded pipe-write mechanism so shutdown cannot hang behind
   a full pipe. A naive blocking WriteFile replacement loses the existing timeout.
   Close handles, check process exit codes, prevent console-window flashes, and
   terminate only the encoder child owned by this session on cancellation.
5. Port exclusive take reservation without weakening collision protection, and
   guard/remove Unix signal handling on Windows. Check UTF-8 to UTF-16 conversion
   consistently for output paths, reservations, logs, and process arguments.
6. Keep the current encoding pipeline: video encodes during capture; the worker
   records float32 stereo PCM, then FFmpeg embeds AAC while stream-copying video
   during SAVING. Temporary files are internal and disappear on success.
   Do not link system FFmpeg libraries into Rack without a deliberate review:
   avoiding symbol collisions with stock VCV Recorder motivated the subprocess.
7. Build and package with the Windows SDK. After the port, the intended commands
   in MinGW64 are `make -j4 RACK_DIR=/c/dev/Rack-SDK` followed by
   `make dist RACK_DIR=/c/dev/Rack-SDK`. The expected artifact is
   `dist/FrameSync-<version>-win-x64.vcvplugin`, containing a native plugin.dll.
   Increment the plugin version for the Windows-capable release and document it.
8. Locate Rack's actual user folder through Help → Open user folder (account for
   overrides). Install the Windows package into its `plugins-win-x64` directory
   and restart Rack. The Linux `*-lin-x64.vcvplugin` package cannot be reused.

## Validation and first use

Adapt and run `tests/core_test.cpp`, `tests/control_test.cpp`,
`tests/check_outputs.py`, `tests/check_controls.py`, and `tests/audio_checks.py`.
Check actual MP4/CSV outputs, not merely compilation. Existing tests cover frame
arithmetic, audio counts/routing, CSV history and offsets, trigger behavior,
auto-rearming, encoder failures, and matching timestamps/durations.

On Windows also verify paths containing spaces and non-ASCII characters, a
missing/stopped encoder, repeated recordings, and clean shutdown/cancellation.
Use a separate Rack test profile (`-u <test-profile>`) for initial UI tests.
Check both region sizes, Windows display scaling, stereo tones, a one-bar TOGGLE
take, and Off versus two-frame CSV compensation. Do not commandeer an audio
device or close the user's active Rack session without coordinating.

A short functional recording is needed to validate the new platform. The user
previously said a new exhaustive audio/video latency benchmark was unnecessary.
Historical visual lag was commonly around two frames on Linux, not a universal
Windows or NYSTHI-scope guarantee. Consult `VALIDATION.md` for what was actually
measured; the old 2.0.1 video-advance measurement is not a 2.1.0 audio test.

On first use, choose a Windows output folder and reselect AREA; Linux paths and
saved screen coordinates should not be assumed valid. Install the Windows
versions of the user's other Rack plugins separately. A custom NYSTHI scope
appearance change from the Linux setup is not part of this FrameSync source;
inspect/transfer that separately if the user wants the same black-background look.

## Transfer, provenance, and privacy

This folder is a clean copy of the existing 2.1.0 source export. Linux originals
and tested packages remain in the original workspace. No SDK, Rack binary,
FFmpeg binary, personal patch, account token, or recorded take belongs in Git.
The included example is the project's synthetic Sync Check patch.

Private repository: https://github.com/gorkulus/FrameSync

Authenticate GitHub separately on Windows; never copy Linux gh credentials.
From a development folder, run `gh repo clone gorkulus/FrameSync`, then open the
cloned FrameSync directory in Codex and read this file. The GitHub CLI and your
Windows Codex session will need access to the private repository. Alternatively,
download its ZIP while signed into GitHub. Keep platform build folders separate.

The source declares GPL-3.0-or-later and includes upstream VCV Recorder attribution
in LICENSE.md. Retain both license files and attribution. Public release and any
bundled FFmpeg distribution are separate future tasks; do not publish now.

## References

- Official build environment, SDK links, and packaging: https://vcvrack.com/manual/Building
- Installation and isolated profile options: https://vcvrack.com/manual/Installing
- Rack user folders: https://vcvrack.com/manual/FAQ
- Windows child process redirection: https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output
- CreateProcessW: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw

Suggested opening prompt for the Windows Codex session:

> Read WINDOWS-HANDOFF.md and the existing source. Help me port FrameSync to
> native Windows, build and validate its Windows VCV Rack plugin, and install it
> for my audio/video/CSV recording workflow. Preserve Linux compatibility and
> the current recording semantics. Keep the project private.
