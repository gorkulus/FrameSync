# FrameSync validation

## Version 2.1.1: native Windows port — 10 October 2026

Built with the official Rack SDK 2.6.6 Windows x64 and MSYS2 MinGW64 GCC 16.2.0;
tested against installed Rack Pro 2.6.6 and FFmpeg 9.0.2 (Gyan essentials).
The source and plugin remain private. Linux runtime tests were not repeated on
this Windows installation; its original process backend and defaults are retained.

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
  passed. Tests exposed and fixed a START/disarm publication race.
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

## Version 2.1.0 validation — 18 September 2026

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

Tested with Rack Pro 2.6.6 on this Linux workstation (RTX 4090), a 48 kHz engine,
and Rack GUI set to 60 fps. The live test used a separate profile and a binary
counter whose displayed code was also sent as CV. Your existing Rack session
remained running. No audio device was opened by the test patch.

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
