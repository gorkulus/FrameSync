"""Check the real files written by the module-level trigger tests."""
import csv
import json
from pathlib import Path
import subprocess
from audio_checks import check_audio

root = Path(Path('build/last-control-test-directory.txt').read_text())
for relative, count, samples in [('bar/take-0001', 120, 96000), ('corrected/take-0001', 2, 1600), ('corrected/take-0002', 1, 1)]:
    base = root / relative
    meta = json.loads(base.with_suffix('.json').read_text())
    rows = list(csv.DictReader(base.with_suffix('.csv').open()))
    frames = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
        '-show_entries', 'frame=best_effort_timestamp_time', '-of', 'json', str(base.with_suffix('.mp4')),
    ]))['frames']
    assert len(rows) == len(frames) == meta['frame_count'] == count
    assert meta['recorded_engine_samples'] == samples
    assert meta['video_advance_frames'] == 0
    assert meta['cv_delay_frames'] == (2 if relative.startswith('corrected/') else 0)
    assert meta['audio_recorded_samples'] == samples
    check_audio(base, count, tone=relative.startswith('bar/'), silence=relative.startswith('corrected/'))
    for n, (row, frame) in enumerate(zip(rows, frames)):
        assert int(row['frame']) == n and int(row['sample_index']) == n * 800
        assert abs(float(row['time_seconds']) - n / 60) < 1e-9
        assert abs(float(frame['best_effort_timestamp_time']) - n / 60) < 1e-6
        for channel in range(16): assert float(row[f'cv{channel + 1:02d}']) == channel + .25
    print(f'{relative}: {count} matching video/CSV frames, {samples} engine samples, all 16 original CV input IDs intact')

mono_audio = None
for delay in [0, 2]:
    base = root / f'mono-delay{delay}' / 'take-0001'
    meta = json.loads(base.with_suffix('.json').read_text())
    rows = list(csv.DictReader(base.with_suffix('.csv').open()))
    assert len(rows) == 30 and meta['cv_history_clamped_rows'] == 0
    assert meta['audio_recorded_samples'] == 24000
    for n, row in enumerate(rows):
        source = (n-delay) * 800
        assert int(row['sample_index']) == n * 800
        assert int(row['cv_sample_index']) == source
        assert abs(float(row['cv01']) - (4800 + source) * .001) < 1e-5
    audio = check_audio(base, 30, tone=True, mono=True)
    if delay == 0:
        mono_audio = audio
    else:
        assert audio == mono_audio
    print(f'mono-delay{delay}: real pre-trigger CV history, correct mono normalization/poly sum, unchanged audio')
