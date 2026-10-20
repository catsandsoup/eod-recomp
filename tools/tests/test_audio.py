"""PSS regressions use authored packets; no retail audio is checked into Git."""
import os
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from audio import _decode_pss_audio, _pss_audio_stream, extract_pss_audio


def packet(payload, stream=0xBD):
    pes = b'\x80\x00\x00' + payload
    return b'\0\0\1' + bytes([stream]) + len(pes).to_bytes(2, 'big') + pes


def ads(body, codec=1, channels=2, interleave=4):
    return (b'SShd' + struct.pack('<7I', 24, codec, 48000, channels, interleave,
                                0xFFFFFFFF, 0xFFFFFFFF) + b'SSbd' + struct.pack('<I', len(body)) + body)


def pss(stream):
    # Header and channel blocks deliberately cross PES boundaries. A fake
    # private-stream prefix inside video must never be scanned as another PES.
    parts = [stream[:3], stream[3:22], stream[22:39], stream[39:]]
    video = packet(b'video\0\0\1\xbdnot audio', stream=0xE0)
    pack = b'\0\0\1\xba' + b'\x44' + b'\0' * 9
    return pack + video + b''.join(packet(b'\xff\xa0\0\0' + p) for p in parts) + b'\0\0\1\xb9'


class PssAudioTests(unittest.TestCase):
    def test_pcm16le_dispatch_and_each_packet_header_removed(self):
        body = struct.pack('<8h', 100, -200, 300, -400, 500, -600, 700, -800)
        stream = ads(body)
        self.assertEqual(_pss_audio_stream(pss(stream)), stream)
        rate, channels, pcm = _decode_pss_audio(pss(stream))
        self.assertEqual((rate, channels), (48000, 2))
        self.assertEqual(struct.unpack('<8h', pcm), (100, 300, -200, -400, 500, 700, -600, -800))

    def test_ps_adpcm_codec_uses_codec_dispatch(self):
        # Filter zero / shift 12 gives exact signed nibble values. Each channel
        # has one frame: +1 throughout on the left, -2 on the right.
        body = bytes([12, 1]) + b'\x11' * 14 + bytes([12, 1]) + b'\xee' * 14
        rate, channels, pcm = _decode_pss_audio(pss(ads(body, codec=0x10, interleave=16)))
        self.assertEqual((rate, channels), (48000, 2))
        self.assertEqual(struct.unpack('<56h', pcm), (1, -2) * 28)

    def test_pcm_mono_and_wav_header(self):
        source = pss(ads(struct.pack('<4h', -1, 20, 300, -4000), channels=1))
        with tempfile.TemporaryDirectory() as tmp:
            inp, out = Path(tmp) / 'movie.pss', Path(tmp) / 'movie.wav'
            inp.write_bytes(source)
            extract_pss_audio(str(inp), str(out))
            with wave.open(str(out), 'rb') as wav:
                self.assertEqual((wav.getnchannels(), wav.getframerate(), wav.getsampwidth()), (1, 48000, 2))
                self.assertEqual(struct.unpack('<4h', wav.readframes(4)), (-1, 20, 300, -4000))

    def test_unknown_codec_is_rejected_before_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            inp, out = Path(tmp) / 'movie.pss', Path(tmp) / 'movie.wav'
            inp.write_bytes(pss(ads(b'\0' * 8, codec=0x99)))
            with self.assertRaisesRegex(ValueError, 'codec'):
                extract_pss_audio(str(inp), str(out))
            self.assertFalse(out.exists())

    def test_reject_bad_extent_transport_and_truncation(self):
        stream = ads(b'\0' * 8)
        cases = [pss(stream[:-1]), pss(stream).replace(b'\xff\xa0\0\0', b'\xff\xa1\0\0'),
                 packet(b'\xff\xa0\0\0' + stream)[:-1], pss(stream + b'\0')]
        for source in cases:
            with self.subTest(source=source[:12]), self.assertRaises(ValueError):
                _decode_pss_audio(source)

    @unittest.skipUnless(os.environ.get('EOD_TEST_RETAIL_AUDIO') == '1', 'Opt in to owner-supplied retail fixture')
    def test_retail_derbba_pcm_extent_and_first_samples(self):
        disc = Path(os.environ.get('EOD_DISC_DATA', str(Path.home() / 'Library/Application Support/TDEODRecomp/disc/DATA')))
        raw = (disc / 'VIDEO/DERBBA.PSS').read_bytes()
        stream = _pss_audio_stream(raw)
        rate, channels, pcm = _decode_pss_audio(raw)
        self.assertEqual((rate, channels), (48000, 2))
        self.assertEqual(len(pcm), struct.unpack_from('<I', stream, 36)[0])
        body = stream[40:]
        # The existing AutoDMA reader consumes 0x200 left, then 0x200 right.
        self.assertEqual(pcm[:8], body[:2] + body[512:514] + body[2:4] + body[514:516])


if __name__ == '__main__':
    unittest.main()
