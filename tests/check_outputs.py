"""Verify encoded MP4 presentation timestamps against the actual CSV outputs."""
import csv
import json
from pathlib import Path
import subprocess
from audio_checks import check_audio

directory = Path('build/last-test-directory.txt').read_text().strip()
for index, expected_count in [(1, 120), (2, 1)]:
    base = Path(directory) / f'take-{index:04d}'
    metadata = json.loads(base.with_suffix('.json').read_text())
    assert metadata['status'] == 'complete', metadata
    with base.with_suffix('.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    video = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
        '-show_entries', 'frame=best_effort_timestamp_time,width,height', '-of', 'json',
        str(base.with_suffix('.mp4')),
    ]))['frames']
    assert len(video) == len(rows) == metadata['frame_count'] == expected_count
    for n, (row, frame) in enumerate(zip(rows, video)):
        assert int(row['frame']) == n
        assert int(row['sample_index']) == n * 800
        assert abs(float(row['time_seconds']) - n / 60) < 1e-9
        assert abs(float(frame['best_effort_timestamp_time']) - n / 60) < 1e-6
        assert frame['width'] == frame['height'] == 512
        expected = n * .125 if index == 1 else -7.25
        assert float(row['comma, quote" column']) == expected
    assert metadata['audio_recorded_samples'] == (96000 if index == 1 else 400)
    check_audio(base, expected_count, tone=index == 1)
    print(f'{base.name}: {len(rows)} video frames and CSV rows, exact samples, matching 60 fps timestamps')
    if index == 1:
        pixels = subprocess.check_output([
            'ffmpeg', '-v', 'error', '-i', str(base.with_suffix('.mp4')),
            '-vf', 'crop=2:2:0:0', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1',
        ])
        assert len(pixels) == 4 * expected_count
        for n in range(expected_count):
            assert abs(pixels[n * 4] - (250 if n % 2 else 10)) <= 2, (n, pixels[n * 4])
            assert int(rows[n]['capture_sample_index']) == n * 800 + (-1 if n % 2 else 1)
        assert metadata['repeated_images'] == 0
        print('Nearest capture preserved every alternating image across +/-1-sample timing jitter')

baseline_video = baseline_audio = None
for delay in [0, 2]:
    count = 30
    base = Path(directory) / f'delay{delay}-0001'
    meta = json.loads(base.with_suffix('.json').read_text())
    rows = list(csv.DictReader(base.with_suffix('.csv').open()))
    frames = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
        '-show_entries', 'frame=best_effort_timestamp_time', '-of', 'json', str(base.with_suffix('.mp4')),
    ]))['frames']
    pixels = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-i', str(base.with_suffix('.mp4')),
        '-vf', 'crop=2:2:0:0', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1',
    ])
    assert meta['status'] == 'complete' and meta['video_advance_frames'] == 0
    assert meta['cv_delay_frames'] == delay and meta['cv_history_clamped_rows'] == 0
    assert meta['audio_recorded_samples'] == (30 * 48001 + 59) // 60
    assert len(rows) == len(frames) == meta['frame_count'] == count
    assert len(pixels) == count * 4
    for n, (row, frame) in enumerate(zip(rows, frames)):
        assert int(row['frame']) == n
        assert int(row['sample_index']) == (n * 48001 + 59) // 60
        source = ((n-delay) * 48001 + 59) // 60
        assert int(row['cv_sample_index']) == source
        assert abs(float(row['cv_time_seconds']) - source / 48001) < 1e-9
        assert int(row['capture_sample_index']) == (n * 48001 + 59) // 60 + 1
        assert float(row['comma, quote" column']) == n - delay
        for ch in range(1, 16): assert float(row[f'cv{ch+1:02d}']) == n - delay + ch
        assert abs(float(frame['best_effort_timestamp_time']) - n / 60) < 1e-6
        assert abs(pixels[n * 4] - (40 + n * 4)) <= 2, (n, pixels[n * 4])
    audio = check_audio(base, count, tone=True)
    if delay == 0:
        baseline_video, baseline_audio = pixels, audio
    else:
        assert pixels == baseline_video and audio == baseline_audio
    print(f'{base.name}: CSV sources shifted by {delay} frames; video/audio identical, stereo tones and 48,001 Hz resampling verified')
assert not list(Path(directory).glob('delay*.partial.*'))
