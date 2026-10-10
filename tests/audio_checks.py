"""Check embedded stereo audio without opening an audio device."""
from array import array
import math
import subprocess
import json


def check_audio(base, frame_count, tone=False, mono=False, silence=False):
    streams = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(base.with_suffix('.mp4')),
    ]))['streams']
    video, = [s for s in streams if s['codec_type'] == 'video']
    audio, = [s for s in streams if s['codec_type'] == 'audio']
    assert audio['codec_name'] == 'aac' and audio['channels'] == 2
    assert int(audio['sample_rate']) == 48000
    assert float(audio['start_time']) == float(video['start_time']) == 0
    assert abs(float(audio['duration']) - frame_count / 60) <= 1 / 48000 + .000001
    decoded = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-i', str(base.with_suffix('.mp4')),
        '-map', '0:a:0', '-f', 'f32le', '-acodec', 'pcm_f32le', 'pipe:1',
    ])
    values = array('f'); values.frombytes(decoded)
    # AAC has codec padding; MP4's presentation duration remains the exact take.
    assert frame_count * 800 <= len(values) // 2 < frame_count * 800 + 1024
    length = frame_count * 800
    if tone:
        for channel, frequency, amplitude in [(0, 440, .5), (1, 440 if mono else 880, .5 if mono else .25)]:
            first, last = min(2048, length // 4), length - min(2048, length // 4)
            error = math.sqrt(sum((values[2*n+channel] - amplitude * math.sin(2*math.pi*frequency*n/48000))**2 for n in range(first, last)) / (last-first))
            assert error < .025, (base, channel, error)
    if mono:
        assert max(abs(values[2*n] - values[2*n+1]) for n in range(length)) < .001
    if silence:
        assert max(abs(value) for value in values) < 1e-7
    return decoded
