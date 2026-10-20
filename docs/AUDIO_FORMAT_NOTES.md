# PSS audio framing and codec evidence

Player success: exported movie WAVs retain the source channel order, sample
values and duration, and play clearly. Byte/format checks establish the first
three properties; listening against the original remains a separate acceptance.

The old extractor treated all movie sound as ADPCM and retained the repeated
four-byte private-stream header after the first PES. Retail DERBBA, COMPACT and
CHAIN use standard Sony SShd codec **1 (PCM16LE)**, not ADPCM: 48,000 Hz, two
channels, 512-byte channel interleave. Every observed private PES begins
`ff a0 00 00`, which is transport framing, not an audio sample.

Reuse/format reference: vgmstream `src/meta/sshd.c`, revision
[`f7f764155b227f683adbab25402fe8f2be0b2ec2`](https://github.com/vgmstream/vgmstream/blob/f7f764155b227f683adbab25402fe8f2be0b2ec2/src/meta/sshd.c)
identifies standard codec 1 as PCM16LE and codec 0x10 as PS-ADPCM. Its interleave
layout and sample-count treatment corroborate the native AutoDMA reader in
`runtime/iop/spu2.cpp`. This tool retains the existing ADPCM decoder for 0x10 and
adds direct PCM handling, rather than applying ADPCM to unknown bytes. Context7
was consulted for FFmpeg demux information; its results did not establish this
Sony substream's codec mapping, so the actual format-specific source above and
retail headers are the decisive evidence.

The bounded supported scope is MPEG-2 PES, the observed four-byte private
substream framing, standard 24-byte SShd header plus SSbd, mono/stereo,
PCM16LE or PS-ADPCM, and complete channel-interleave blocks. Other codec/header
variants, truncated packets and inconsistent declared extents fail explicitly.
Packet boundaries may split Sony headers or channel data. The demuxer skips
whole non-audio PES payloads, preventing embedded start-code bytes in video
from being mistaken for audio packets. No media bytes are stored in Git.

On 2026-10-20, all six audio tests passed with the optional owner-supplied DERBBA
fixture enabled. Synthetic tests cover exact signed PCM values and channel
order across PES boundaries, mono WAV metadata, exact simple ADPCM samples,
unsupported codecs and malformed/truncated extents. Retail in-memory decoding
produced these declared PCM byte counts (all 48 kHz stereo):

| Source | PCM bytes | Stereo frames |
| --- | ---: | ---: |
| DERBBA.PSS | 12,024,832 | 3,006,208 |
| COMPACT.PSS | 11,672,576 | 2,918,144 |
| CHAIN.PSS | 12,205,056 | 3,051,264 |

Run portable tests with `python3 -m unittest tools.tests.test_audio -v`. Enable
`EOD_TEST_RETAIL_AUDIO=1` to check the owner's DERBBA file; `EOD_DISC_DATA` may
select a separate DATA directory. Tests run on the CPU, without playing sound
or launching the game. They do not establish every PSS variant or audible parity.
