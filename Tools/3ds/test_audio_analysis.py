"""Host checks that native PCM evidence corruption cannot pass validation."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from analyze_audio_pcm import analyze, fnv


class AudioEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.wav = Path(self.directory.name) / 'pcm.wav'
        self.meta = self.wav.with_suffix('.json')
        # Independent one-block stereo fixture with different left/right values.
        pcm = struct.pack('<hh', 1000, -2000) * 800
        data = (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16) +
                b'data' + struct.pack('<I', len(pcm)) + pcm)
        self.wav.write_bytes(data)
        self.record = dict(schema=1, observation='pre_device_mixer', host_playback_verified=False,
                           format='signed16_le_interleaved_stereo', sample_rate=48000,
                           frames_per_block=800, wav_bytes=len(data), wav_fnv1a32=fnv(data),
                           wav_readback_verified=True, captured_blocks=1, requested_block_count=1,
                           requested_start_block=2, first_captured_block=2, end_block_exclusive=3,
                           filled_blocks=3, submitted_queue_calls=3, observed_completed_blocks=2,
                           capture_submitted_queue_calls=1, capture_observed_completed_blocks=0,
                           range_valid=True, sample_values=1600, output='ndsp', ndsp_result=0,
                           nonzero_samples=[800,800], peak_abs_s16=[1000,2000],
                           clipped_or_full_scale_samples=[0,0], queue_empty_observations=0)
        self.save()

    def save(self):
        self.meta.write_text(json.dumps(self.record))

    def test_valid_signal_is_not_playback_acceptance(self):
        result = analyze(self.wav, self.meta)
        self.assertEqual(result['status'], 'verified')
        self.assertEqual(result['channels'][1]['rms_s16'], 2000)
        self.assertFalse(result['channels_identical'])
        self.assertFalse(result['host_playback_verified'])
        self.assertFalse(result['hardware_audio_verified'])

    def test_corrupt_payload_rejected(self):
        data = bytearray(self.wav.read_bytes()); data[-1] ^= 1
        self.wav.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'WAV bytes/hash'):
            analyze(self.wav, self.meta)

    def test_truncation_and_trailing_payload_rejected(self):
        original = self.wav.read_bytes()
        for changed in [original[:-1], original+b'\0']:
            with self.subTest(length=len(changed)):
                self.wav.write_bytes(changed)
                with self.assertRaises(ValueError): analyze(self.wav, self.meta)

    def test_impossible_queue_counters_rejected(self):
        self.record['observed_completed_blocks'] = 4; self.save()
        with self.assertRaisesRegex(ValueError, 'queue counters'):
            analyze(self.wav, self.meta)

    def test_stale_channel_metadata_rejected(self):
        self.record['nonzero_samples'] = [0,0]; self.save()
        with self.assertRaisesRegex(ValueError, 'nonzero counts'):
            analyze(self.wav, self.meta)


if __name__ == '__main__':
    unittest.main()
