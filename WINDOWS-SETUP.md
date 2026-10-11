# FrameSync 2.1.1: native Windows setup

This port targets Windows x64 / Rack 2. Linux retains its POSIX encoder backend.
The original `WINDOWS-HANDOFF.md` remains as the pre-port requirements record.

## Install and first use

1. Put `FrameSync-2.1.1-win-x64.vcvplugin` in the actual Rack user folder's
   `plugins-win-x64` directory and restart Rack. Rack extracts the package.
2. Install a native Windows FFmpeg build with libx264 and AAC. FFmpeg stays
   external; neither its libraries nor its binaries are part of this package.
   See https://ffmpeg.org/download.html for Windows build links.
3. Search the module browser for **FrameSync Recorder**. An explicit FFmpeg path
   can be chosen with **Choose FFmpeg executable…** in its context menu and is
   saved in the patch. With no explicit setting, `FRAMESYNC_FFMPEG` is used,
   followed by `ffmpeg.exe` on PATH. To set a default for new modules, create
   `<Rack user folder>/FrameSync-ffmpeg.txt` containing one UTF-8 executable path.
   Explicit saved patch settings take precedence over that per-user default.
4. Choose a Windows output folder, select SIZE, and click AREA to select a new
   crop. Crop sizes are physical framebuffer pixels. Do this again when moving
   a Linux patch to Windows or changing display scaling/window dimensions.
5. Connect audio to L/MONO and R and your CV sources to POLY CV or CV01–16.
   Keep Rack's audio engine running, normally through your Audio module/device.
   Set the GUI frame rate to 60 fps or higher, ARM, and wait for READY.
6. REC toggles recording. TOGGLE accepts one rising edge to start and another
   to stop; GATE and separate START/STOP are also available. Wait for SAVED.

Missing/broken FFmpeg produces an error message and preserves take diagnostics.
An interrupted take keeps its partial files. Successful takes remove temporary
PCM/video files after producing the MP4, CSV, and JSON metadata.

## Rebuild in MSYS2 MinGW64

Use the official Windows x64 Rack SDK and VCV's documented MSYS2 environment:
https://vcvrack.com/manual/Building . Keep source/SDK paths free of spaces.

```sh
make -j4 RACK_DIR=/c/dev/Rack-SDK
make dist RACK_DIR=/c/dev/Rack-SDK
```

The artifact is `dist/FrameSync-2.1.1-win-x64.vcvplugin`. The Windows link adds
`opengl32`, uses the SDK's `libRack.dll.a`, and statically links compiler runtimes.
No SDK, runtime executable, license key, encoder, or test take belongs in Git.

For tests, place FFmpeg/ffprobe and Python on PATH. The Windows SDK supplies an
import library; module-level tests also need `libRack.dll` and its dependencies
from an installed Rack on their runtime PATH. For example:

```sh
export FRAMESYNC_FFMPEG='C:/tools/ffmpeg/bin/ffmpeg.exe'
export PATH='/c/tools/ffmpeg/bin:/c/path/to/python:'"$PATH"
make test RACK_DIR=/c/dev/Rack-SDK
make test-controls RACK_DIR=/c/dev/Rack-SDK RACK_RUNTIME_DIR='/c/Program Files/VCV/Rack2Pro'
make test-windows RACK_DIR=/c/dev/Rack-SDK
```

`FRAMESYNC_FFMPEG` must contain a native Windows UTF-8 path, such as
`C:/tools/ffmpeg/bin/ffmpeg.exe`, rather than an MSYS-only `/c/…` path.

The optional `FRAMESYNC_TEST_HARNESS` build automatically arms the synthetic
Sync Check patch and exits its isolated Rack instance. It is excluded from the
installed production DLL. Rack Pro's isolated profile needs its existing Pro
license to advance the engine. Use `-u <separate-profile>` and no Audio module
for device-free tests; never test against the user's autosave/settings.

## Implementation

`Encoder.hpp` shares the unchanged video/mux arguments and executable resolution.
`EncoderPosix.hpp` preserves the Linux process/pipe logic and SIGPIPE protection.
`EncoderWindows.hpp` uses CreateProcessW with an explicit inherited-handle list,
Windows argument quoting, a hidden console, overlapped named-pipe writes with a
ten-second deadline, and cancellation that terminates only the owned child.
All take filesystem operations use UTF-8-to-native paths; reservations use
CreateFileW with CREATE_NEW. The worker's START/disarm publication race is fixed
for both platforms. Engine recording semantics, slugs, and existing ports remain.

See `VALIDATION.md` for the Windows functional results and limits. A universal
scope/audio latency calibration is outside these functional tests.
