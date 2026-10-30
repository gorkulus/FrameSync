# FrameSync validation

macOS is not yet supported.

## Integrated 2.1.2 Linux validation — 29 October 2026

Integrated the platform code and tests from Windows preview commit
`512c2a9e8050b62d001f04c67b53486e8d32e7d8` with the cleaned Linux documentation
and repository metadata. Original licenses and Andrew Belt/VCV Recorder
attribution are unchanged. The following is fresh Linux evidence, separate from
the historical reports below. Native Windows execution, Windows package building,
and interactive Windows AREA tests were **not run** in this integration.

### Deterministic START/disarm regressions

`make test-races` compiles the real CaptureSession with test-only callbacks and
condition-variable gates. Each wait is bounded; no sleep or probabilistic stress
is used to arrange the race interleavings. Hooks are absent from production builds.

The tests force four schedules:

1. Recording is visible but the Start event has not yet been queued; disarm runs
   before the worker's cancellation check.
2. The worker has finished its event drain; Start is queued afterward while the
   take is still Recording when the cancellation check occurs.
3. The same event-drain gap with Start/frame/audio/Stop already queued and the
   take already Finishing before the worker checks cancellation.
4. begin has read the armed flag; the worker has read Ready after disarm; begin
   claims the take before the worker's cancellation transition. This exposes the
   preview guard's separate state-read/decision race.

All four schedules now save the accepted take and end Idle without auto-rearming.
Every take contains 1,601 start-inclusive/stop-exclusive engine audio samples,
three 60 fps MP4 frames and CSV rows, followed by a fresh 800-sample/one-frame take
on the same session with distinct CV data. No stale events contaminate it.
The suite also checks cancellation winning against a pending begin, disarm before
any accepted start, empty cleanup, and shutdown while Start publication is paused.
Interrupted accepted takes preserve partial files/error metadata; unstarted
shutdown cleans up without claiming a successful recording. Re-arm after final
shutdown is rejected. Ten successful MP4/CSV/JSON sets are decoded/validated.

`make test-races-negative` builds only temporary source copies under build/:
restoring the original cancellation predicate causes the first three schedules
to fail with lost accepted takes; restoring a snapshot-only guard causes the
fourth to fail. All four expected failures were observed. Timeouts/crashes do not
count as successful reproduction. The production source is never changed by it.

### Transition and ownership review

START claims Ready → Recording by compare/exchange. Unstarted cancellation now
claims Ready/Priming → Finishing on that same atomic, so only one can win; a stale
state snapshot cannot authorize cleanup. Disarm clears auto-rearm intent but
does not itself stop an already accepted take (the module supplies STOP).
The worker keeps draining Recording/Finishing until Stop is consumed.

Shutdown/error closes admission before waiting for any in-flight begin to publish.
Its release/acquire handshake preserves a claimed Start for diagnostics. Only the
worker waits; engine begin/end remain nonblocking. End also claims Recording →
Finishing atomically before flushing audio and publishing Stop, avoiding a late
state overwrite during cleanup. Event/audio generation IDs are fixed by the
engine's accepted begin instead of being reread during a later queue push.
Queues retain one producer/consumer and their release/acquire publication order.

### Commands and scope

All commands below passed with the official Rack SDK 2.6.6 Linux x64, GCC 16.2.1,
Python 3.14.7, and FFmpeg/ffprobe n9.0.2 (libx264 and AAC available):

```sh
make -j4 RACK_DIR=/absolute/path/to/Rack-SDK
make test-races RACK_DIR=/absolute/path/to/Rack-SDK
make test-races-negative RACK_DIR=/absolute/path/to/Rack-SDK
make test RACK_DIR=/absolute/path/to/Rack-SDK
make test-controls RACK_DIR=/absolute/path/to/Rack-SDK
make test-posix RACK_DIR=/absolute/path/to/Rack-SDK
make dist RACK_DIR=/absolute/path/to/Rack-SDK
git diff --check
```

Core and module-control output checks passed: stereo audio, mono normalization,
poly sums, silence, resampling, exact frame/sample boundaries, trigger priority,
held/busy edges, CV-only delay, pre-trigger history and saved-setting migration.
Constructor defaults (UTF-8/CRLF) and saved ffmpegPath overrides, including empty
strings, passed in a temporary profile. Encoder selection preserves module/default
path → FRAMESYNC_FFMPEG → platform fallback precedence.

Linux subprocess checks passed for 512/1024 video, UTF-8/spaced executable and
output paths, and simultaneous exclusive reservations. Missing and exited children
failed explicitly. A deliberately stalled child hit the write deadline in 10.009 s;
cancellation interrupted a blocked write in 0.101 s. CaptureSession shutdown
completed within the test's three-second bound. Actual MP4/AAC streams, CSVs,
metadata and successful temporary-file cleanup passed the validators.

`make dist` produced `FrameSync-2.1.2-lin-x64.vcvplugin`. Archive inspection
confirmed plugin.so, plugin.json, README, VALIDATION, WINDOWS-SETUP, both license
files, and the example patch; no deleted handoff, Windows DLL, or FFmpeg binary.
Relative Markdown file links resolve inside the package. Production symbols and
strings contain neither deterministic test hooks nor the automatic GUI harness.

The packaged plugin loaded as FrameSync 2.1.2 in Rack Pro 2.6.6's screenshot mode
using a separate temporary profile; Recorder and SyncTest rendered and Rack exited
normally. This was a production load/panel check, not a new live capture or AREA
interaction test. Installed plugins and the normal Rack profile were untouched.
The compiler emitted the existing Rack SDK Port::setChannels array-bound warning
from SyncTest; the build succeeded. Windows tests/backends remain in the source,
but native Windows checks and cross-compilation were not performed here.

## Historical Windows 2.1.1 report — 10 October 2026

Built with the official Rack SDK 2.6.6 Windows x64 and MSYS2 MinGW64 GCC 16.2.0;
tested against installed Rack Pro 2.6.6 and FFmpeg 9.0.2 (Gyan essentials).
This section retains the Windows-port commit
`512c2a9e8050b62d001f04c67b53486e8d32e7d8` report. These commands and live captures
were not rerun on Windows during the 29 October Linux integration.

`make test`, `make test-controls`, and `make test-windows` passed. The output
validators decoded the actual MP4/AAC streams and inspected CSV/metadata:

- Stereo tones, polyphonic summing, L-to-R mono normalization, silence, and
  48,001 Hz resampling passed. Video and audio start at zero; presentation
  duration matches the exact 60 fps frame count.
- A one-bar TOGGLE take retained 96,000 engine samples, 120 video frames and
  CSV rows. One-sample/partial-frame takes retained their sample counts and
  padded audio only to the final video-frame boundary.
- Off versus two-frame CSV compensation changed only CSV sources: decoded
  audio and video were identical for identical input. Pre-trigger history,
  timestamp columns, saved-setting migration, and stable input IDs passed.
- STOP priority, held/busy/disarmed edges, repeated recordings, and auto-rearm
  passed. Tests exposed a START/disarm publication race and added a snapshot guard.
  The integrated 2.1.2 tests later found and replaced a remaining race in that guard.
- Both 512 and 1024 pixel sizes recorded successfully with spaces/non-ASCII
  characters in output and executable paths. Concurrent reservations produced
  distinct take numbers, without collisions or leftover successful temp files.
- Missing, exited, and stalled encoder children failed clearly. Pipe writes
  timed out within twelve seconds; cancellation/shutdown completed within three
  seconds in the deliberate stalled-child tests. Failed takes kept diagnostics.

Short isolated Rack GUI captures ran on the NVIDIA RTX 4090 at Windows 150%
display scaling (Rack automatic pixel ratio), using the synthetic Sync Check
patch at 50% patch zoom so barcode pixels could be decoded. No audio device was
opened. This checks real GL_FRONT framebuffer reads and physical crop dimensions,
rather than only synthetic worker buffers. These recordings contain silence;
stereo routing/tone validation used the real module processor tests above.

The initial 512/Off take produced 180 matching video/CSV frames and 144,000 audio
samples in three seconds, without capture-queue overflow. Its barcode showed
mostly two frames of render lag, with occasional three-frame observations. This
is a functional observation, not a universal calibration for Windows or NYSTHI.
Historical Linux measurements below are retained as separate results.

Four three-second GUI takes covered both 512/1024 sizes and Off/two-frame CSV
delay. Each produced 180 matched video/CSV frames, 144,000 engine audio samples,
48 kHz stereo AAC with aligned three-second presentation, and zero capture-queue
overflows. Barcode decoding passed for all 16 CV channels. Off showed mostly
two-frame render lag; compensation brought most measured rows to zero lag,
with occasional one-frame differences. These short takes do not establish an
exhaustive latency benchmark or calibrate other scopes/plugins.

The interactive AREA mouse/keyboard workflow and arbitrary third-party scope
patches still need the user's first-use check. The GUI harness selected the crop
through the same framebuffer-coordinate scaling, without exercising mouse input.

## Historical Linux validation — 18 September 2026

## Version 2.1.0: stereo audio and CSV-only correction

Functional tests used the actual Rack module processor plus FFmpeg-generated
MP4 files. No audio device was opened and no new live visual/audio latency
benchmark was performed.

- MP4s contain H.264 video and stereo AAC at 48 kHz. Both streams start at zero;
  audio presentation durations match the 60 fps video durations.
- Distinct left/right test tones survived recording with expected amplitude and
  channel routing. Polyphonic input summing and L-to-R mono normalization passed.
- A two-second trigger interval captured exactly 96,000 engine samples, 120 video
  frames, and 120 CSV rows. One-sample and half-frame takes retained the gate
  sample count while padding audio to the final video-frame boundary.
- A 48,001 Hz source successfully resampled to 48 kHz, with correct timestamps.
- Off and two-frame CSV compensation produced byte-identical decoded audio and
  matching video pixels for identical inputs; only CSV source values shifted.
- Actual module tests verified all delayed CV samples, including real pre-trigger
  history. Ring-buffer wrap and sample-rate resets were checked separately.
- A missing audio sample or non-finite audio/CV explicitly failed the take without
  publishing a successful MP4/CSV pair. Automatic rearming remained functional.
- The packaged production plugin loaded in an isolated Rack Pro 2.6.6 profile;
  its rendered panel showed both audio jacks with readable labels.
- Trigger priority/held/busy-edge regressions passed; existing input IDs 0–20
  remain unchanged. L/R are appended as IDs 21/22. Saved correction values migrate
  to CSV-only delay, and new-format values take precedence.

The older measurements below concern the previous implementation. In particular,
2.0.1 advanced video and captured a post-roll; 2.1.0 instead delays CSV and leaves
audio/video unchanged. Its historical 1,800-frame result is not a new benchmark
of 2.1.0.

## Historical live-test setup (2.0.x)

Tested with Rack Pro 2.6.6 on Linux (RTX 4090), a 48 kHz engine,
and Rack GUI set to 60 fps. The live test used a separate profile and a binary
counter whose displayed code was also sent as CV. No audio device was opened
by the test patch.

## Version 2.0.2: TOGGLE recording input

Tests exercised the actual module processor and Rack SDK trigger detectors,
then verified the resulting MP4 and CSV files:

- Two TOGGLE rising edges 96,000 samples apart at 48 kHz recorded one 4/4 bar at
  120 BPM: exactly 120 MP4 frames and 120 CSV rows, with matching 60 fps timestamps.
- Holding a trigger high caused only one toggle. A high input held through
  arming/rearming did not start another take without a fresh rising edge.
- Simultaneous START/STOP left a ready recorder waiting and stopped a recording.
  STOP also overrode simultaneous TOGGLE while ready.
- A simultaneous manual REC click and TOGGLE edge toggled only once.
- Pulses during correction post-roll were ignored, with no delayed start after
  saving; a fresh pulse after READY successfully started the next take.
- TOGGLE stopping preserved video correction and exact gate duration, including
  a short take. All 16 CV channels were checked in the resulting files.
- Existing input IDs 0–19 remain unchanged; TOGGLE is appended at ID 20.

These are processor/encoding tests using generated pixels, not a new scope
latency benchmark. The previous live visual timing results are retained below.

## Version 2.0.1: optional two-frame correction

A 30-second live test at 512×512, 60 fps, and 48 kHz used the new **2 frames
(33.3 ms)** compensation setting. All **1,800 decoded visual counter values
exactly matched their corresponding CSV values**, including the first and last
frames. There were no reused captures or capture-queue overflows. All 16 CV
channels, video timestamps, sample indices, and file lengths passed verification.
The selection also persisted correctly in the saved Rack patch.

The implementation uses later screenshots while retaining the original CV
samples and output duration. It captures a brief tail after STOP. Automated
encoding tests verified correction on every frame at 48,001 Hz (a sample rate
not divisible by 60), including a take shorter than one video frame. A missing
post-roll causes an explicit error, without publishing a shortened final pair.
Off-mode tests also passed unchanged.

This is one live counter test, not a guarantee of exact alignment under every
load or with NYSTHI's scope. Compensation is fixed; a smaller actual display
lag will make the corrected image early, and a larger lag will leave it late.
Off remains the default, with 1–4-frame options for comparison.

## Version 2.0.0 baseline: compensation Off

| Preset | Duration | MP4 frames / CSV rows | Reused captures | Capture queue overflows |
|---|---:|---:|---:|---:|
| 512 × 512 | 10 s | 600 / 600 | 5 (0.83%) | 0 |
| 1024 × 1024 | 60 s | 3,600 / 3,600 | 22 (0.61%) | 0 |

Every decoded video presentation timestamp matched `frame / 60`, and every CSV
sample index matched `frame * 800`. All 16 recorded CV channels matched their
expected counter or bit values. There were no missing or extra rows.

The visual counter normally lagged its CSV value by **two frames (33.3 ms)**.
After startup, observed lag ranged from one to three frames (16.7–50 ms).
The final second averaged two frames in both tests. These results show bounded
visual timing variation during these takes, not accumulating file-timeline drift.
They do not establish zero GUI jitter or guarantee the latency of NYSTHI's scope.

A reused capture means the same framebuffer read was used for another output
frame; it does not mean a CSV row was dropped. The output remains exactly 60 fps.
The CSV's capture-offset columns measure when pixels were read, not when a scope
internally drew them. The measured two-frame offset is therefore **not** applied
as a correction to arbitrary scope recordings.

These were isolated test patches without an audio master. A complex musical
patch, audio driver, window visibility, and system load can change the display
latency and repeat rate. The included Sync Check module lets you assess those
conditions. The results are suitable for a first artistic evaluation, not a
claim of sample-synchronous oscilloscope rendering.

## Automated checks

- Exact frame-boundary arithmetic over a simulated ten hours at 22,050, 44,100,
  48,000, 48,001, and 96,000 samples/second.
- Concurrent producer/consumer queue ordering across 100,000 events.
- Two successive encoded takes, automatic rearming, correct filenames, and CSV
  quoting, including negative CV values and values above 10 V.
- Actual MP4 presentation timestamps, frame counts, dimensions, and recorded CV.
- Alternating dark/light images captured one sample before/after successive frame
  boundaries: every image preserved, without artificial repeats at the boundary.
- Invalid/duplicate CSV names rejected. Non-finite CV terminates the take with
  diagnostics and preserves partial output instead of publishing a successful pair.
- Production binary excludes the automatic arming/exit integration-test harness.

Versions 2.0.x recorded video + CV CSV only. Version 2.1.0 adds stereo audio.
