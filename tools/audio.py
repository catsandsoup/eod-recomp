#!/usr/bin/env python3
"""Audio and speech tool for Test Drive: Eve of Destruction.

Full decompiler and compiler for:
1. PS2 SPU2 ADPCM codec (Sony 16-byte blocks with hardware filter prediction).
2. SFX0 sound effects (.sfx <-> .wav).
3. SPEECH.BNK sound banks (extraction to .wav + manifest.json, compilation back to .bnk).
4. MPEG-2 / PSS stream music extraction (.pss -> .wav).
"""

import argparse
import io
import json
import math
import os
import struct
import sys
import wave

# SPU2 hardware filter table (matches hardware & runtime/iop/spu2.cpp)
K_FILTER = [
    [0, 0],
    [60, 0],
    [115, -52],
    [98, -55],
    [122, -60]
]


def decode_spu2_adpcm(adpcm_data: bytes) -> list:
    """Decodes raw SPU2 ADPCM byte stream into a list of 16-bit signed PCM samples."""
    if len(adpcm_data) % 16:
        raise ValueError("ADPCM stream must contain complete 16-byte blocks")
    samples = []
    h1, h2 = 0, 0
    total_blocks = len(adpcm_data) // 16

    for blk in range(total_blocks):
        block = adpcm_data[blk * 16 : (blk + 1) * 16]
        header = block[0]
        shift = header & 15
        filt = (header >> 4) & 7
        if shift > 12:
            shift = 9
        if filt > 4:
            filt = 0

        f0, f1 = K_FILTER[filt]

        for i in range(28):
            byte_val = block[2 + i // 2]
            nib = (byte_val >> ((i & 1) * 4)) & 15
            # Sign-extend 4-bit nibble
            if nib >= 8:
                nib -= 16

            s = (nib << 12) >> shift
            s += (h1 * f0 + h2 * f1 + 32) >> 6
            s = min(max(s, -32768), 32767)
            h2 = h1
            h1 = s
            samples.append(s)

    return samples


def encode_spu2_adpcm(pcm_samples: list, loop_start: int = -1, loop_end: int = -1) -> bytes:
    """Encodes a list of 16-bit signed PCM samples into SPU2 ADPCM byte blocks."""
    out = bytearray()
    h1, h2 = 0, 0
    total_blocks = (len(pcm_samples) + 27) // 28
    padded = list(pcm_samples) + [0] * (total_blocks * 28 - len(pcm_samples))

    for blk in range(total_blocks):
        block_samples = padded[blk * 28 : (blk + 1) * 28]
        best_err = float("inf")
        best_filt = 0
        best_shift = 0
        best_nibbles = [0] * 28
        best_h1, best_h2 = h1, h2

        # Evaluate candidate filters and shifts to minimize quantization error
        for filt in range(5):
            f0, f1 = K_FILTER[filt]
            for shift in range(13):
                cur_h1, cur_h2 = h1, h2
                err = 0
                nibs = []
                for s_val in block_samples:
                    pred = (cur_h1 * f0 + cur_h2 * f1 + 32) >> 6
                    diff = s_val - pred
                    # Scale to 4-bit quantization grid
                    n = int(round((diff << shift) / 4096.0))
                    n = min(max(n, -8), 7)
                    nibs.append(n)

                    # Reconstruct sample
                    s_dec = (n << 12) >> shift
                    s_dec += pred
                    s_dec = min(max(s_dec, -32768), 32767)
                    cur_h2 = cur_h1
                    cur_h1 = s_dec

                    d = s_val - s_dec
                    err += d * d
                    if err >= best_err:
                        break

                if err < best_err:
                    best_err = err
                    best_filt = filt
                    best_shift = shift
                    best_nibbles = nibs
                    best_h1, best_h2 = cur_h1, cur_h2

        h1, h2 = best_h1, best_h2
        header_byte = (best_filt << 4) | (best_shift & 15)

        # Flag byte: bit 0 = loop end, bit 1 = loop repeat, bit 2 = loop start
        flag_byte = 0
        sample_start_idx = blk * 28
        sample_end_idx = (blk + 1) * 28

        if loop_start != -1 and sample_start_idx <= loop_start < sample_end_idx:
            flag_byte |= 0x04
        if loop_end != -1 and sample_start_idx <= loop_end < sample_end_idx:
            flag_byte |= 0x01 | 0x02
        elif blk == total_blocks - 1:
            flag_byte |= 0x01  # end of stream

        out.append(header_byte)
        out.append(flag_byte)

        # Pack 28 nibbles into 14 bytes
        for i in range(14):
            low = best_nibbles[i * 2] & 15
            high = best_nibbles[i * 2 + 1] & 15
            out.append(low | (high << 4))

    return bytes(out)


def write_wav(path: str, samples: list, sample_rate: int = 22050, channels: int = 1):
    """Writes 16-bit PCM samples to a standard WAV file."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with wave.open(path, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(sample_rate)
        raw_pcm = struct.pack(f"<{len(samples)}h", *samples)
        wf.writeframes(raw_pcm)


def read_wav(path: str) -> tuple:
    """Reads a WAV file and returns (sample_rate, channels, list_of_samples)."""
    with wave.open(path, "rb") as wf:
        channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        rate = wf.getframerate()
        nframes = wf.getnframes()
        raw_data = wf.readframes(nframes)

    if sampwidth != 2:
        # Convert 8-bit to 16-bit if needed
        raise ValueError(f"WAV must be 16-bit PCM (got {sampwidth * 8}-bit)")

    total_samples = len(raw_data) // 2
    samples = list(struct.unpack(f"<{total_samples}h", raw_data))
    return rate, channels, samples


def sfx_to_wav(sfx_path: str, wav_path: str):
    """Decompiles an SFX0 file to a standard 16-bit WAV file."""
    with open(sfx_path, "rb") as f:
        data = f.read()

    if len(data) < 32:
        raise ValueError(f"{sfx_path} too small for SFX0 header")

    data_len, hdr_size, channels, unk1 = struct.unpack("<IIII", data[:16])
    rate, max_rate, unk2, unk3 = struct.unpack("<IIII", data[16:32])

    if hdr_size != 32 or channels != 1 or rate <= 0 or hdr_size + data_len > len(data):
        raise ValueError("Only complete 32-byte-header mono SFX0 files are supported")

    adpcm_data = data[hdr_size : hdr_size + data_len]
    samples = decode_spu2_adpcm(adpcm_data)
    write_wav(wav_path, samples, sample_rate=rate, channels=channels)
    print(f"Decompiled {sfx_path} -> {wav_path} ({len(samples)} samples, {rate} Hz)")


def wav_to_sfx(wav_path: str, sfx_path: str, target_rate: int = None):
    """Compiles a 16-bit mono WAV file to an SFX0 file."""
    rate, channels, samples = read_wav(wav_path)
    if channels != 1:
        # Downmix stereo to mono
        mono_samples = []
        for i in range(0, len(samples), channels):
            mono_samples.append(sum(samples[i : i + channels]) // channels)
        samples = mono_samples
        channels = 1

    if target_rate is not None and target_rate != rate:
        raise ValueError("Sample-rate conversion is not implemented; resample the WAV first")
    adpcm = encode_spu2_adpcm(samples)
    data_len = len(adpcm)
    hdr_size = 32
    actual_rate = target_rate or rate

    hdr = struct.pack("<IIIIIIII", data_len, hdr_size, channels, 0x1CFFF, actual_rate, 44100, 0x40002, 0)
    os.makedirs(os.path.dirname(os.path.abspath(sfx_path)), exist_ok=True)
    with open(sfx_path, "wb") as f:
        f.write(hdr + adpcm)

    print(f"Compiled {wav_path} -> {sfx_path} ({len(adpcm) + 32} bytes, {actual_rate} Hz)")


def extract_speech_bank(bank_path: str, out_dir: str):
    """Extracts SPEECH.BNK into a directory of .wav files and manifest.json."""
    os.makedirs(out_dir, exist_ok=True)
    with open(bank_path, "rb") as f:
        head = f.read(16)
        magic, ver, cue_count, unk = struct.unpack("<4sIII", head)
        if magic != b"Bank":
            raise ValueError(f"{bank_path} is not a Bank file (magic={magic})")

        cues = []
        for _ in range(cue_count):
            cue_id, sec_off, byte_sz = struct.unpack("<III", f.read(12))
            cues.append({
                "id": cue_id,
                "offset_sectors": sec_off,
                "size_bytes": byte_sz,
            })

        print(f"Extracting {len(cues)} cues from {bank_path}...")
        manifest = {
            "version": ver,
            "cues": cues,
        }
        with open(os.path.join(out_dir, "manifest.json"), "w") as mf:
            json.dump(manifest, mf, indent=2)

        # Extract audio for each cue
        for c in cues:
            f.seek(c["offset_sectors"] * 2048)
            raw_adpcm = f.read(c["size_bytes"])
            pcm = decode_spu2_adpcm(raw_adpcm)
            wav_name = f"cue_{c['id']:04d}.wav"
            write_wav(os.path.join(out_dir, wav_name), pcm, sample_rate=22050, channels=1)

    print(f"Successfully extracted {len(cues)} cues to {out_dir}")


def compile_speech_bank(manifest_path: str, in_dir: str, bank_out_path: str):
    """Compiles a directory of cue WAVs and manifest.json into SPEECH.BNK."""
    with open(manifest_path, "r") as mf:
        manifest = json.load(mf)

    cues = manifest["cues"]
    cue_count = len(cues)
    ver = manifest.get("version", 1)

    # 16-byte header + cue_count * 12 bytes
    dir_size = 16 + cue_count * 12
    # Aligned to 16 sectors (32,768 bytes / 0x8000)
    data_start_sector = max(16, (dir_size + 2047) // 2048)

    cur_sector = data_start_sector
    bank_data = bytearray(cur_sector * 2048)
    compiled_cues = []

    for c in cues:
        wav_name = f"cue_{c['id']:04d}.wav"
        wav_path = os.path.join(in_dir, wav_name)
        if os.path.exists(wav_path):
            rate, channels, samples = read_wav(wav_path)
            if channels != 1 or rate != 22050:
                raise ValueError("Speech cue WAVs must be mono 22050 Hz")
            adpcm = encode_spu2_adpcm(samples)
        else:
            raise FileNotFoundError(wav_path)

        sz = len(adpcm)
        compiled_cues.append((c["id"], cur_sector, sz))
        bank_data.extend(adpcm)
        # Pad to next sector
        pad_needed = (2048 - (len(adpcm) % 2048)) % 2048
        bank_data.extend(b"\0" * pad_needed)
        cur_sector += (sz + 2047) // 2048

    # Write header
    struct.pack_into("<4sIII", bank_data, 0, b"Bank", ver, cue_count, 0)
    for i, (cid, sec_off, sz) in enumerate(compiled_cues):
        struct.pack_into("<III", bank_data, 16 + i * 12, cid, sec_off, sz)

    os.makedirs(os.path.dirname(os.path.abspath(bank_out_path)), exist_ok=True)
    with open(bank_out_path, "wb") as f:
        f.write(bank_data)

    print(f"Compiled {bank_out_path}: {cue_count} cues, {len(bank_data)} bytes ({cur_sector} sectors)")


def _pss_audio_stream(data: bytes) -> bytes:
    """Demux the observed Sony private substream; never scan inside PES payloads."""
    chunks = []
    pos = 0
    while pos < len(data):
        start = data.find(b"\x00\x00\x01", pos)
        if start < 0:
            break
        if start + 4 > len(data):
            raise ValueError("Truncated MPEG start code")
        sid = data[start + 3]
        if sid == 0xB9:  # program end
            break
        if sid == 0xBA:  # MPEG-2 pack header, plus stuffing bytes
            if start + 14 > len(data) or data[start + 4] & 0xC0 != 0x40:
                raise ValueError("Only complete MPEG-2 PSS pack headers are supported")
            pos = start + 14 + (data[start + 13] & 7)
            if pos > len(data):
                raise ValueError("Truncated MPEG pack stuffing")
            continue
        if start + 6 > len(data):
            raise ValueError("Truncated MPEG packet header")
        length = int.from_bytes(data[start + 4:start + 6], "big")
        end = start + 6 + length
        if not length or end > len(data):
            raise ValueError("Unbounded or truncated MPEG packet")
        if sid == 0xBD:
            if length < 3 or data[start + 6] & 0xC0 != 0x80:
                raise ValueError("Only MPEG-2 private audio PES headers are supported")
            payload_start = start + 9 + data[start + 8]
            if payload_start + 4 > end:
                raise ValueError("Truncated PSS private audio header")
            payload = data[payload_start:end]
            # The retail movie substream repeats this four-byte transport
            # header on EVERY PES, including packets that split SShd/SSbd.
            if payload[:4] != b"\xff\xa0\x00\x00":
                raise ValueError("Unsupported PSS private substream framing")
            chunks.append(payload[4:])
        pos = end
    if not chunks:
        raise ValueError("No supported PSS audio stream found")
    return b"".join(chunks)


def _decode_pss_audio(data: bytes):
    """Return (rate, channels, interleaved PCM16LE) from the supported ADS body.

    Codec 1 is PCM16LE and 0x10 is PS-ADPCM (Sony definitions corroborated by
    vgmstream src/meta/sshd.c). EOD's AutoDMA also consumes PCM16LE in 0x200-byte
    channel blocks, see runtime/iop/spu2.cpp. Other codec/layout variants fail.
    """
    stream = _pss_audio_stream(data)
    if len(stream) < 40 or stream[:4] != b"SShd" or stream[32:36] != b"SSbd":
        raise ValueError("Expected complete standard SShd/SSbd audio stream")
    header_size, codec, rate, channels, interleave = struct.unpack_from("<5I", stream, 4)
    if header_size != 24 or rate <= 0 or channels not in (1, 2):
        raise ValueError("Unsupported Sony audio header")
    if codec not in (1, 0x10):
        raise ValueError(f"Unsupported Sony audio codec {codec:#x}")
    alignment = 2 if codec == 1 else 16
    if not interleave or interleave % alignment:
        raise ValueError("Invalid audio channel interleave")
    body_size = struct.unpack_from("<I", stream, 36)[0]
    body = stream[40:]
    if len(body) != body_size or not body or len(body) % (interleave * channels):
        raise ValueError("Incomplete or unsupported Sony audio body extent")
    channel_data = [bytearray() for _ in range(channels)]
    for offset in range(0, len(body), interleave * channels):
        for channel in range(channels):
            begin = offset + channel * interleave
            channel_data[channel].extend(body[begin:begin + interleave])
    if codec == 0x10:
        channel_data = [struct.pack(f"<{len(samples)}h", *samples)
                        for samples in (decode_spu2_adpcm(raw) for raw in channel_data)]
    pcm = bytearray(len(channel_data[0]) * channels)
    # Two byte lanes per sample; avoid building millions of Python integers
    # for PCM movie audio or retaining transport bytes in the channel streams.
    for channel, raw in enumerate(channel_data):
        pcm[channel * 2::channels * 2] = raw[0::2]
        pcm[channel * 2 + 1::channels * 2] = raw[1::2]
    return rate, channels, bytes(pcm)


def extract_pss_audio(pss_path: str, wav_path: str):
    """Extract supported Sony PCM16LE/PS-ADPCM PSS audio to an ordinary WAV."""
    with open(pss_path, "rb") as handle:
        rate, channels, pcm = _decode_pss_audio(handle.read())
    os.makedirs(os.path.dirname(os.path.abspath(wav_path)), exist_ok=True)
    with wave.open(wav_path, "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(pcm)
    print(f"Extracted {pss_path} -> {wav_path} ({len(pcm) // (2 * channels)} frames, {rate} Hz)")


def inspect_audio(path: str):
    """Inspects an audio asset."""
    with open(path, "rb") as f:
        head = f.read(64)

    if head.startswith(b"Bank"):
        magic, ver, count = struct.unpack("<4sII", head[:12])
        print(f"SPEECH.BNK: version={ver}, cues={count}")
    elif path.endswith(".sfx") or path.endswith(".sf_"):
        sz, hsz, ch, _, rate = struct.unpack("<IIIII", head[:20])
        print(f"SFX0: size={sz} bytes, channels={ch}, sample_rate={rate} Hz")
    elif path.endswith(".pss"):
        print(f"PSS Video/Audio Container: size={os.path.getsize(path)} bytes")


def main():
    parser = argparse.ArgumentParser(description="Audio, Speech & Music tool for Test Drive: Eve of Destruction")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    # sfx2wav
    p_s2w = subparsers.add_parser("sfx2wav", help="Decompile .sfx to .wav")
    p_s2w.add_argument("input", help="Input .sfx file")
    p_s2w.add_argument("output", help="Output .wav file")

    # wav2sfx
    p_w2s = subparsers.add_parser("wav2sfx", help="Compile .wav to .sfx")
    p_w2s.add_argument("input", help="Input .wav file")
    p_w2s.add_argument("output", help="Output .sfx file")
    p_w2s.add_argument("--rate", type=int, default=None, help="Target sample rate (default: WAV rate)")

    # bnk-extract
    p_bex = subparsers.add_parser("bnk-extract", help="Extract SPEECH.BNK to WAVs and manifest.json")
    p_bex.add_argument("input", help="Input SPEECH.BNK")
    p_bex.add_argument("out_dir", help="Output directory")

    # bnk-compile
    p_bcp = subparsers.add_parser("bnk-compile", help="Compile WAVs and manifest.json to SPEECH.BNK")
    p_bcp.add_argument("manifest", help="Input manifest.json")
    p_bcp.add_argument("in_dir", help="Directory containing cue WAVs")
    p_bcp.add_argument("output", help="Output SPEECH.BNK file")

    # pss-extract
    p_pss = subparsers.add_parser("pss-extract", help="Extract music from PSS video stream")
    p_pss.add_argument("input", help="Input .pss file")
    p_pss.add_argument("output", help="Output .wav file")

    # inspect
    p_ins = subparsers.add_parser("inspect", help="Inspect audio container")
    p_ins.add_argument("input", help="Input audio container")

    args = parser.parse_args()
    if args.cmd == "sfx2wav":
        sfx_to_wav(args.input, args.output)
    elif args.cmd == "wav2sfx":
        wav_to_sfx(args.input, args.output, args.rate)
    elif args.cmd == "bnk-extract":
        extract_speech_bank(args.input, args.out_dir)
    elif args.cmd == "bnk-compile":
        compile_speech_bank(args.manifest, args.in_dir, args.output)
    elif args.cmd == "pss-extract":
        extract_pss_audio(args.input, args.output)
    elif args.cmd == "inspect":
        inspect_audio(args.input)


if __name__ == "__main__":
    main()
