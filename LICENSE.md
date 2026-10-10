# FrameSync

FrameSync source is licensed under GNU GPL version 3 or later. See LICENSE-GPLv3.txt.

The framebuffer capture approach and recorder module structure are adapted from
VCV Recorder, copyright © 2019–2020 Andrew Belt, also GPL-3.0-or-later.
Upstream: https://github.com/VCVRack/VCV-Recorder (v2 branch).
Reference revision: defcf7890bc9630e288ca7cfcc4dd998eb314ccf.

FrameSync's timing, capture queues, CSV recorder, process-based encoder backend,
selection interface, panel drawing, tests, and Sync Check module are new work.
No VCV Recorder panel artwork, VCV logos, or cursor artwork are included.
Rack's standard port components are supplied by the user's Rack installation.

FFmpeg is a separate, unmodified system program and is not included in the plugin.
Its own license and codec configuration apply to that program.
