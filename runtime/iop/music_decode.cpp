#include "music_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

namespace music {

namespace {
std::string lower_ext(const std::string &path) {
    size_t d = path.find_last_of('.');
    if (d == std::string::npos) return "";
    std::string e = path.substr(d);
    for (char &c : e) c = (char)tolower((unsigned char)c);
    return e;
}
constexpr size_t kChunk = 16384;
}  // namespace

bool is_music_extension(const std::string &e) {
    static const char *const kExt[] = {".mp3", ".m4a", ".m4p", ".m4b", ".aac", ".wav", ".wave", ".aif", ".aiff", ".aifc",
                                       ".caf", ".flac", ".alac", ".mp4", ".ogg", ".oga", ".opus", ".wma", ".mka",
                                       ".ac3", ".au", ".snd", ".amr", ".3gp", ".mp2"};
    for (const char *x : kExt)
        if (e == x) return true;
    return false;
}

#ifdef __APPLE__
static std::string os_status(OSStatus s) {
    char b[64];
    unsigned char c[4] = {(unsigned char)(s >> 24), (unsigned char)(s >> 16), (unsigned char)(s >> 8), (unsigned char)s};
    bool printable = true;
    for (unsigned char x : c) printable &= (x >= 32 && x < 127);
    if (printable) snprintf(b, sizeof b, "error '%c%c%c%c'", c[0], c[1], c[2], c[3]);
    else snprintf(b, sizeof b, "error %d", (int)s);
    return b;
}

DecodeResult decode_file(const std::string &path, const DecodeSink &sink) {
    DecodeResult res;
    const std::string ext = lower_ext(path);
    if (ext == ".m4p") {
        res.status = DecodeStatus::Protected;
        res.reason = "Protected (copy-protected iTunes purchase)";
        return res;
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), (CFIndex)path.size(), false);
    if (!url) { res.reason = "Bad path"; return res; }
    ExtAudioFileRef f = nullptr;
    OSStatus st = ExtAudioFileOpenURL(url, &f);
    CFRelease(url);
    if (st != noErr) {
        const bool unsupported = st == kAudioFileUnsupportedFileTypeError || st == kAudioFileUnsupportedDataFormatError ||
                                 st == kAudioFileInvalidFileError || st == kAudioFileUnspecifiedError;
        const bool prot = st == kAudioFilePermissionsError;
        if (prot) { res.status = DecodeStatus::Protected; res.reason = "Protected (cannot be decoded)"; }
        else if (unsupported) { res.status = DecodeStatus::Unsupported; res.reason = "Unsupported format (" + os_status(st) + ")"; }
        else res.reason = "Cannot open (" + os_status(st) + ")";
        return res;
    }
    AudioStreamBasicDescription out = {};
    out.mSampleRate = 48000;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mBitsPerChannel = 16;
    out.mChannelsPerFrame = 2;
    out.mFramesPerPacket = 1;
    out.mBytesPerFrame = 4;
    out.mBytesPerPacket = 4;
    st = ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out);
    if (st != noErr) {
        ExtAudioFileDispose(f);
        res.status = DecodeStatus::Unsupported;
        res.reason = "Unsupported format (" + os_status(st) + ")";
        return res;
    }
    std::vector<int16_t> buf(kChunk * 2);
    uint64_t total = 0;
    for (;;) {
        AudioBufferList bl;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = 2;
        bl.mBuffers[0].mDataByteSize = (UInt32)(kChunk * 4);
        bl.mBuffers[0].mData = buf.data();
        UInt32 frames = (UInt32)kChunk;
        st = ExtAudioFileRead(f, &frames, &bl);
        if (st != noErr) {
            ExtAudioFileDispose(f);
            res.reason = "Decode failed (" + os_status(st) + ")";
            return res;
        }
        if (frames == 0) break;
        total += frames;
        if (!sink(buf.data(), frames)) { ExtAudioFileDispose(f); res.reason = "Cancelled"; return res; }
    }
    ExtAudioFileDispose(f);
    if (total == 0) { res.reason = "No audio in file"; return res; }
    res.status = DecodeStatus::Ok;
    return res;
}

#elif defined(_WIN32)

DecodeResult decode_file(const std::string &path, const DecodeSink &sink) {
    using Microsoft::WRL::ComPtr;
    DecodeResult res;
    if (lower_ext(path) == ".m4p") {
        res.status = DecodeStatus::Protected;
        res.reason = "Protected (copy-protected iTunes purchase)";
        return res;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    std::wstring wp(path.begin(), path.end());
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    wp.assign((size_t)std::max(n, 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], n);
    ComPtr<IMFSourceReader> rd;
    HRESULT hr = MFCreateSourceReaderFromURL(wp.c_str(), nullptr, &rd);
    if (FAILED(hr)) { res.status = DecodeStatus::Unsupported; res.reason = "Unsupported format"; return res; }
    ComPtr<IMFMediaType> mt;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    mt->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    mt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt.Get());
    if (FAILED(hr)) { res.status = DecodeStatus::Unsupported; res.reason = "Unsupported format"; return res; }
    uint64_t total = 0;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) { res.reason = "Decode failed"; return res; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> mb;
        sample->ConvertToContiguousBuffer(&mb);
        BYTE *data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
            total += len / 4;
            const bool keep = sink(reinterpret_cast<const int16_t *>(data), len / 4);
            mb->Unlock();
            if (!keep) { res.reason = "Cancelled"; return res; }
        }
    }
    if (!total) { res.reason = "No audio in file"; return res; }
    res.status = DecodeStatus::Ok;
    return res;
}

#else  // portable fallback: PCM WAV only

DecodeResult decode_file(const std::string &path, const DecodeSink &sink) {
    DecodeResult res;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) { res.reason = "Cannot open"; return res; }
    std::vector<uint8_t> d;
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) d.insert(d.end(), tmp, tmp + n);
    fclose(fp);
    if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) {
        res.status = DecodeStatus::Unsupported;
        res.reason = "Unsupported format (only WAV without the OS decoder)";
        return res;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    size_t pcm_bytes = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t sz;
        memcpy(&sz, &d[p + 4], 4);
        if (!memcmp(&d[p], "fmt ", 4) && p + 24 <= d.size()) {
            memcpy(&fmt, &d[p + 8], 2); memcpy(&ch, &d[p + 10], 2); memcpy(&rate, &d[p + 12], 4); memcpy(&bits, &d[p + 22], 2);
        } else if (!memcmp(&d[p], "data", 4)) {
            pcm = &d[p + 8];
            pcm_bytes = std::min<size_t>(sz, d.size() - p - 8);
            break;
        }
        p += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || bits != 16 || ch < 1 || ch > 2 || !rate || !pcm) {
        res.status = DecodeStatus::Unsupported;
        res.reason = "Unsupported WAV (need 16-bit PCM)";
        return res;
    }
    const size_t in_frames = pcm_bytes / (2 * ch);
    const uint64_t out_frames = (uint64_t)in_frames * 48000 / rate;
    std::vector<int16_t> out;
    out.reserve(kChunk * 2);
    for (uint64_t o = 0; o < out_frames; o++) {
        const double pos = (double)o * rate / 48000.0;
        const size_t i0 = (size_t)pos, i1 = std::min(i0 + 1, in_frames - 1);
        const double fr = pos - (double)i0;
        for (int c = 0; c < 2; c++) {
            const int cc = ch == 1 ? 0 : c;
            int16_t a, b;
            memcpy(&a, pcm + (i0 * ch + cc) * 2, 2);
            memcpy(&b, pcm + (i1 * ch + cc) * 2, 2);
            out.push_back((int16_t)(a + (b - a) * fr));
        }
        if (out.size() >= kChunk * 2) {
            if (!sink(out.data(), out.size() / 2)) { res.reason = "Cancelled"; return res; }
            out.clear();
        }
    }
    if (!out.empty() && !sink(out.data(), out.size() / 2)) { res.reason = "Cancelled"; return res; }
    if (!out_frames) { res.reason = "No audio in file"; return res; }
    res.status = DecodeStatus::Ok;
    return res;
}
#endif

}  // namespace music
