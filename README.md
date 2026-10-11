# FrameSync Recorder 2.1.1 — Linux and Windows

Native Windows support uses CreateProcessW, cancellable pipe writes, and Unicode
filesystem paths. See [Windows setup](WINDOWS-SETUP.md) and `VALIDATION.md`.

A separately named Rack 2 plugin based on VCV Recorder's framebuffer capture
approach, with 60 fps H.264 video, embedded stereo AAC audio, and 16-channel
frame-onset CSV recording.

Linux defaults to `/usr/bin/ffmpeg`; Windows resolves `ffmpeg.exe` on PATH.
An explicit executable can be selected in the module's context menu and is saved
with the patch. `FRAMESYNC_FFMPEG` overrides the default when no explicit setting
is selected. Windows also accepts a per-user default in `FrameSync-ffmpeg.txt`
inside Rack's user folder (one UTF-8 executable path).
It never invokes a shell or links FFmpeg libraries into Rack's process.

Install the matching `.vcvplugin` package in Rack's `plugins-lin-x64` or
`plugins-win-x64` folder, then restart
Rack. Search the module browser for **FrameSync**. It installs alongside VCV
Recorder and does not replace it. The source and panel are GPL-3.0-or-later.
See `VALIDATION.md` for current functional checks and historical visual timing measurements.

## Capture a take

1. Add **FrameSync Recorder**. Set Rack's GUI frame rate to 60 fps or higher.
2. Right-click and choose an output folder. SIZE switches 512×512 / 1024×1024.
3. Click AREA, move the square over the scope, then click to select. Arrow keys
   nudge one physical pixel, Shift+arrows ten. Enter confirms; Escape cancels.
   The full selected square must fit inside Rack's framebuffer.
4. Patch CV into the numbered inputs, or use POLY CV for up to 16 channels.
   A connected numbered input overrides its corresponding poly channel.
   Column names and the filename prefix are editable in the context menu.
   Column names must be unique; reserved timing-column names cannot be reused.
5. Patch your mix into **L/MONO** and **R** on the bottom row. If only L/MONO
   is patched, it feeds both audio channels. With neither patched, the MP4 is silent.
6. Click ARM. Wait for READY (also a 10 V output). TOGGLE accepts one trigger
   to start and the next to stop. Alternatively, gate high starts a take and
   gate low stops it, or use separate START/STOP triggers. REC toggles manually.
   Choose one control method; leave GATE unpatched when using trigger inputs.
7. SAVED emits a pulse after both files are finalized. The recorder then
   prepares the next numbered take. Wait for READY before starting again.
   DISARM cancels readiness, or stops an active take and disables auto-rearm.

Outputs are `take-0001.mp4`, `take-0001.csv`, and diagnostic `take-0001.json`.
The encoder's log has the same basename. A recording interrupted by an error
keeps available `.partial.*` files and an error manifest. Temporary audio/video
files are merged automatically during SAVING and removed after a successful take.
Files are reserved together, and existing takes are never intentionally replaced.
No recording or arming is resumed automatically when opening a saved Rack patch.

The crop is relative to Rack's window in physical framebuffer pixels. Moving
the window is fine. Panning/zooming the patch changes what occupies the crop;
reselect it as needed. Resizing the window so the crop no longer fits stops
the take with an error. Keep menus, cables, tooltips, and other Rack overlays
outside the selected area. The selection overlay is removed before arming.

## One-bar or one-sequence takes with TOGGLE

Patch a pulse at the beginning of each bar (or complete sequence) into **TOGGLE**.
Arm and wait for READY: the next rising edge starts a take, and the following
rising edge stops it. This records the interval between those two pulses.
For example, one 4/4 bar at 120 BPM is two seconds: 120 video frames at 60 fps.
At tempos where a bar is not a whole number of video frames, the final frame
rounds the video duration up by less than 1/60 second; trigger timing remains
sample-accurate. The existing CSV/MP4 pairing and delay compensation still apply.

If the pulse continues, the third edge starts another take once READY, and the
fourth stops it. This alternates recording and waiting across successive bars;
it does not split every bar into an uninterrupted series of recordings.
For a single take, disarm after it stops or stop sending TOGGLE pulses.

A held-high signal toggles only once. Pulses while disarmed, preparing, or saving
are ignored and never queued for later. Wait for READY before a fresh start.
If START and STOP rise on the same engine sample, **STOP wins**: a waiting
recorder stays waiting, and an active recording stops. STOP also takes priority
over TOGGLE. TOGGLE and a manual REC click on the same sample toggle only once.

Existing saved cable connections are preserved when updating to version 2.1.0.
The bottom output jacks move to make room for audio; their connections stay intact.

## Stereo audio

Audio comes from the module's **L/MONO** and **R** inputs, not desktop sound or
an audio device's monitor. Split the same mixer outputs to your listening output
and FrameSync. Recording does not depend on which audio interface is selected.
Each audio jack sums its polyphonic channels, as VCV Recorder does. An unpatched
R copies L. ±10 V maps to digital full scale; larger signals clip. A normal ±5 V
audio signal records at half scale. CV inputs retain their raw voltage range.

Audio is sampled on every Rack engine tick from the start trigger, up to but
excluding the stop trigger's sample. Video uses the same recording origin and
engine sample clock. The MP4 contains 48 kHz stereo AAC at 320 kb/s; other engine
rates are resampled automatically. No microphone or hardware loopback is used.

The encoder writes video during recording. After STOP it encodes the captured
audio and embeds it in the MP4, copying the already encoded video without another
video encode. Wait for **SAVED** before using the files or quitting Rack. This is
part of the recorder's save operation; no external editing step is needed.

## Optional CSV delay compensation

Disarm, then right-click → **CSV delay compensation**. Choose **Off** (default)
or **1–4 frames** (16.7–66.7 ms). Two frames (33.3 ms) is a useful comparison with
the historical display-lag measurement. The panel shows `CSV +2f` for this setting.
The selection is saved in the patch. An existing 2.0.x correction setting keeps
its selected amount when loading, but now applies to CSV only.

**Audio and video timing stay unchanged.** With two frames selected, CSV row n
contains the CV from two frames before that audio/video frame. This delays CV
changes to line up with a display that draws them two frames late. The recorder
retains recent CV history, so the opening rows use real values from before the
start trigger. If the module has only just been added or its sample rate changed,
unavailable history is held at the earliest available value and flagged in the
CSV. Let the engine run briefly before starting for complete history.

Unlike version 2.0.x, compensation no longer advances the video or captures a
post-roll after STOP. You still wait for SAVED while the files finish writing.
This remains a fixed offset, not automatic scope-latency tracking. Compare Off
and two frames in your actual patch; variable GUI delay can still vary alignment.

## Timing and data

`frame` is zero-based and maps to decoded MP4 presentation order. At 60 fps,
`time_seconds = frame / 60`. `sample_index` identifies the audio/video frame onset,
relative to the recording trigger: `ceil(frame * engine_sample_rate / 60)`.
At 48 kHz this is exactly 800 samples per frame.

With compensation Off, CV is sampled at that onset. With K frames of CSV delay,
its source sample is `ceil((frame - K) * engine_sample_rate / 60)`. The appended
`cv_sample_index` and `cv_time_seconds` columns identify that source, including
negative values for pre-trigger history. `cv_history_clamped` is 1 only when the
requested history was unavailable and the earliest available value was held.
The JSON records `cv_delay_frames`, `cv_delay_ms`, and clamped-row count.

CV values are raw volts without gain or normalization. Disconnected channels
contain zero; values outside ±10 V are preserved. There is exactly one CSV row
per video frame. Existing column names and frame numbering are unchanged.

Video selects the framebuffer read nearest each scheduled frame onset, using
one capture of lookahead. The `capture_sample_index` and `capture_offset_ms`
columns describe that read relative to the frame onset, independently of CSV
correction. They do not measure when the scope internally updated its pixels.
`repeated_image` flags reuse of the same capture, not two identical-looking
captures. Slow GUI updates may cause repeats; timestamps are never shifted to
hide them. JSON diagnostics also include audio sample counts and clipping counts.

Audio/CV capture never writes files, allocates memory, or waits on the encoder.
Video capture runs on the GUI thread; a background worker handles file writing
and encoding. Queue overflow, missing audio samples, non-finite input, a changed
engine rate during recording, or encoder failure marks the take as an error
instead of silently changing its timeline.

A stop between video-frame boundaries leaves a final whole video frame, so the
clip duration may exceed the gate by less than 1/60 second. Audio stops at the
exact gate boundary; only the remaining fraction of that last frame is padded
with silence. Video, audio, and CSV then share the same clip duration.

## Sync Check

Connect Recorder READY → Sync Check READY IN, Sync Check GATE OUT → Recorder
GATE, and Sync Check POLY CV → Recorder POLY CV. Capture its barcode display,
arm the recorder, and press RUN ONCE. The first CV column records the integer
display code; the next twelve carry its binary bits. This lets you measure the
visual capture delay against the recorded CV without LOGAN20.

`examples/Sync-check.vcv` contains this wiring. Choose an output folder and AREA
over the display, click ARM, then press RUN ONCE. The patch does not use an audio
device. Open it as a separate patch after saving your current work.

The scope's own drawing and Rack's display updates add latency. A fixed 60 fps
file and exact CV timestamps do not guarantee that a GUI scope draws every
intermediate state. A busy or hidden Rack window can produce repeated images.
Keep Rack visible while capturing. Use the Sync Check to assess the current
system load. Apply a measured correction explicitly in the recorder's context menu.

## Build

Obtain the official Rack 2.6.6 Linux x64 SDK and set `RACK_DIR`:

```sh
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
make test RACK_DIR=/absolute/path/to/Rack-SDK
python3 tests/check_outputs.py
make test-controls RACK_DIR=/absolute/path/to/Rack-SDK
make dist RACK_DIR=/absolute/path/to/Rack-SDK
```

Build requires a C++17 compiler, make, jq, and the Rack SDK. Packaging also uses
zstd. Runtime encoding requires FFmpeg with libx264 and AAC encoders. This initial implementation
uses Linux process APIs and is not a Windows/macOS build.
