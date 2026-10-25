#!/usr/bin/env python3
"""Verify bounded native PCM evidence; signal metrics do not certify playback."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def bounded_read(path, maximum):
    with Path(path).open('rb') as source:
        data = source.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError(f'{Path(path).name}: evidence exceeds bound')
    return data


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def analyze(wav_path, json_path):
    data = bounded_read(wav_path, 1024 * 1024)
    raw_json = bounded_read(json_path, 8192)
    meta = json.loads(raw_json)
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:16] != b'WAVEfmt ' or data[36:40] != b'data':
        raise ValueError('Expected native 44-byte PCM WAV header')
    riff_size, = struct.unpack_from('<I', data, 4)
    fmt_size, kind, channels, rate, byte_rate, align, bits = struct.unpack_from('<IHHIIHH', data, 16)
    payload_size, = struct.unpack_from('<I', data, 40)
    if (fmt_size, kind, channels, rate, byte_rate, align, bits) != (16, 1, 2, 48000, 192000, 4, 16):
        raise ValueError('Unexpected native PCM format')
    if riff_size + 8 != len(data) or payload_size + 44 != len(data) or payload_size % 4:
        raise ValueError('Truncated, padded or malformed native WAV')
    errors = []
    def check(condition, label):
        if not condition:
            errors.append(label)
    def integer(key, low=0, high=2**64-1):
        value = meta.get(key)
        if type(value) is not int or not low <= value <= high:
            raise ValueError('Invalid integer field: ' + key)
        return value
    check(meta.get('schema') == 1, 'schema')
    check(meta.get('observation') == 'pre_device_mixer', 'observation')
    check(meta.get('host_playback_verified') is False, 'host playback cannot be inferred')
    check(meta.get('format') == 'signed16_le_interleaved_stereo', 'format')
    check(meta.get('sample_rate') == rate and meta.get('frames_per_block') == 800, 'block format')
    check(meta.get('wav_bytes') == len(data) and meta.get('wav_fnv1a32') == fnv(data), 'WAV bytes/hash')
    check(meta.get('wav_readback_verified') is True, 'guest readback')
    captured = integer('captured_blocks', high=327)
    requested = integer('requested_block_count', low=1, high=327)
    start = integer('requested_start_block', high=0xffffffff)
    fills = integer('filled_blocks')
    submitted = integer('submitted_queue_calls')
    completed = integer('observed_completed_blocks')
    cap_submitted = integer('capture_submitted_queue_calls')
    cap_completed = integer('capture_observed_completed_blocks')
    check(captured == min(requested, max(0, fills-start)), 'capture interval')
    check(meta.get('first_captured_block') == start and meta.get('end_block_exclusive') == start+captured, 'block range')
    check(meta.get('range_valid') is bool(captured), 'range validity')
    check(completed <= submitted <= fills and cap_completed <= cap_submitted <= captured, 'queue counters')
    check(payload_size == captured * 800 * 4 and meta.get('sample_values') * 2 == payload_size, 'sample count')
    check(meta.get('output') in ('ndsp', 'muted_missing_component', 'uninitialized'), 'output state')
    if meta.get('output') == 'ndsp':
        check(meta.get('ndsp_result') == 0, 'NDSP initialization result')
    if meta.get('output') == 'muted_missing_component':
        check(meta.get('ndsp_result') == 0xd880a7fa and submitted == completed == 0, 'muted fallback')
    stats = []
    frames = payload_size // 4
    pairs = list(struct.iter_unpack('<hh', data[44:]))
    for channel in range(2):
        samples = [pair[channel] for pair in pairs]
        stats.append({'nonzero': sum(value != 0 for value in samples),
                      'peak_abs_s16': max(map(abs, samples), default=0),
                      'full_scale_samples': sum(value in (-32768,32767) for value in samples),
                      'mean_s16': sum(samples)/frames if frames else 0,
                      'rms_s16': math.sqrt(sum(value*value for value in samples)/frames) if frames else 0})
    check(meta.get('nonzero_samples') == [s['nonzero'] for s in stats], 'nonzero counts')
    check(meta.get('peak_abs_s16') == [s['peak_abs_s16'] for s in stats], 'peaks')
    check(meta.get('clipped_or_full_scale_samples') == [s['full_scale_samples'] for s in stats], 'endpoints')
    if errors:
        raise ValueError('PCM evidence mismatch: ' + '; '.join(errors))
    return {'status': 'verified', 'scope': 'pre-device mixer only',
            'host_playback_verified': False, 'hardware_audio_verified': False,
            'wav_sha256': hashlib.sha256(data).hexdigest(),
            'metadata_sha256': hashlib.sha256(raw_json).hexdigest(),
            'wav_bytes': len(data), 'duration_seconds': frames/rate,
            'capture_complete': captured == requested, 'captured_blocks': captured,
            'output': meta['output'], 'filled_blocks': fills,
            'submitted_queue_calls': submitted, 'observed_completed_blocks': completed,
            'queue_empty_observations': integer('queue_empty_observations'),
            'channels': stats, 'channels_identical': all(left == right for left,right in pairs),
            'limits': 'Signal, queue calls and observed DONE do not establish audible output, content correctness or physical underrun duration.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav', type=Path)
    parser.add_argument('--metadata', type=Path)
    args = parser.parse_args()
    result = analyze(args.wav, args.metadata or args.wav.with_suffix('.json'))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
