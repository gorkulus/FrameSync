"""Verify the files produced by Linux path/reservation tests."""
import csv
import json
from pathlib import Path
import subprocess
from audio_checks import check_audio

root = Path(Path('build/last-posix-test-directory.txt').read_text(encoding='utf-8'))
for size in (512, 1024):
    for index in (1, 2):
        base = root / f'size{size}-{index:04d}'
        metadata = json.loads(base.with_suffix('.json').read_text())
        rows = list(csv.DictReader(base.with_suffix('.csv').open()))
        frames = json.loads(subprocess.check_output([
            'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
            '-of', 'json', str(base.with_suffix('.mp4')),
        ]))['frames']
        assert metadata['status'] == 'complete'
        assert len(rows) == len(frames) == 1
        assert frames[0]['width'] == frames[0]['height'] == size
        assert int(rows[0]['frame']) == 0 and float(rows[0]['cv01']) == 3.25
        assert metadata['audio_recorded_samples'] == 800
        check_audio(base, 1)
assert not list(root.glob('size*.partial.*'))
assert not list(root.glob('*.reserve'))
assert json.loads((root / 'missing-0001.json').read_text())['status'] == 'error'
assert json.loads((root / 'shutdown-0001.json').read_text())['status'] == 'error'
print('Linux MP4/CSV pairs verified for both sizes and simultaneous Unicode-path reservations.')
