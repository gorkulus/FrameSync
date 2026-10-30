"""Validate files from deterministically scheduled CaptureSession recordings."""
import csv
import json
from pathlib import Path
import subprocess
from audio_checks import check_audio

root = Path(Path('build/last-race-directory.txt').read_text())
saved = 0
for metadata in root.rglob('*.json'):
    meta = json.loads(metadata.read_text())
    base = metadata.with_suffix('')
    if meta['status'] == 'error':
        assert metadata.parent.name == 'shutdown-claimed'
        assert 'shutdown' in meta['error'] and not base.with_suffix('.mp4').exists()
        continue
    fresh = base.name.startswith('fresh-')
    count, samples, cv = (1, 800, -7.5) if fresh else (3, 1601, 3.25)
    assert meta['recorded_engine_samples'] == meta['audio_recorded_samples'] == samples
    assert meta['frame_count'] == count and meta['video_advance_frames'] == meta['cv_delay_frames'] == 0
    rows = list(csv.DictReader(base.with_suffix('.csv').open()))
    assert len(rows) == count
    frames = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
        '-show_entries', 'frame=best_effort_timestamp_time', '-of', 'json', str(base.with_suffix('.mp4')),
    ]))['frames']
    assert len(frames) == count
    for n, (row, frame) in enumerate(zip(rows, frames)):
        assert int(row['frame']) == n and int(row['sample_index']) == int(row['cv_sample_index']) == n * 800
        assert float(row['cv01']) == cv
        assert abs(float(frame['best_effort_timestamp_time']) - n / 60) < 1e-6
    check_audio(base, count)
    assert not list(base.parent.glob(base.name + '.partial.*'))
    assert not base.with_suffix('.reserve').exists()
    saved += 1
assert saved == 10, saved
print(f'{saved} complete MP4/CSV/JSON takes verified, plus interrupted-Start diagnostics; exact samples and fresh generations')
