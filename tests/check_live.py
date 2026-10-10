"""Decode Sync Check's barcode and compare every MP4 frame with its CV row.

Usage: python3 tests/check_live.py /path/to/take-0001 [/path/to/take-0002 ...]
The selected area must start at the display's top-left corner, at 100% zoom.
"""
import collections
import csv
import json
from pathlib import Path
import statistics
import subprocess
import sys

for argument in sys.argv[1:]:
    base = Path(argument)
    metadata = json.loads(base.with_suffix('.json').read_text())
    assert metadata['status'] == 'complete', metadata
    with base.with_suffix('.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    frames = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
        '-show_entries', 'frame=best_effort_timestamp_time,width,height', '-of', 'json',
        str(base.with_suffix('.mp4')),
    ]))['frames']
    assert len(frames) == len(rows) == metadata['frame_count']
    for n, (row, frame) in enumerate(zip(rows, frames)):
        assert int(row['frame']) == n
        assert int(row['sample_index']) == (n * metadata['sample_rate'] + 59) // 60
        assert abs(float(row['time_seconds']) - n / 60) < 1e-9
        assert abs(float(frame['best_effort_timestamp_time']) - n / 60) < 1e-6
        assert frame['width'] == frame['height'] == metadata['width']
        delay = metadata.get('cv_delay_frames', 0)
        # Pre-trigger history may contain the previous run's held final code.
        code = (n - delay) % 4096 if n >= delay else int(float(row['cv01']))
        assert float(row['cv01']) == code
        for channel in range(1, 16):
            bit = (channel - 1) % 12
            assert float(row[f'cv{channel + 1:02d}']) == (10 if code & (1 << bit) else 0)
    raw = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-i', str(base.with_suffix('.mp4')),
        '-vf', 'crop=500:2:0:100', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1',
    ])
    assert len(raw) == 1000 * len(rows)
    codes = [sum((raw[n * 1000 + 26 + 40 * bit] > 128) << bit for bit in range(12))
             for n in range(len(rows))]
    lag = [(int(float(row['cv01'])) - code + 2048) % 4096 - 2048 for row, code in zip(rows, codes)]
    # Startup is expected to show the pre-trigger display. Assess steady state separately.
    stable = lag[60:] or lag
    result = {
        'take': base.name, 'frames': len(rows), 'size': metadata['width'],
        'video_advance_frames': metadata.get('video_advance_frames', 0),
        'cv_delay_frames': metadata.get('cv_delay_frames', 0),
        'video_csv_timestamps_match': True, 'all_16_cv_channels_checked': True,
        'counter_and_12_bit_channels_match': True,
        'visual_lag_histogram_frames': dict(sorted(collections.Counter(lag).items())),
        'steady_state_lag_min_frames': min(stable), 'steady_state_lag_max_frames': max(stable),
        'first_second_after_warmup_mean_lag': statistics.mean(stable[:60]),
        'last_second_mean_lag': statistics.mean(stable[-60:]),
        'reused_captures': metadata['repeated_images'],
        'capture_queue_overflows': metadata['capture_queue_overflows'],
    }
    print(json.dumps(result, indent=2))
    base.with_suffix('.validation.json').write_text(json.dumps(result, indent=2) + '\n')
