// music_decode.h - decode any audio file the OS can play to 48 kHz interleaved stereo
// int16. macOS: AudioToolbox ExtAudioFile (MP3, AAC/M4A, ALAC, FLAC, AIFF, WAV, CAF).
// Windows: Media Foundation source reader (compiled behind _WIN32; not tested here).
// Elsewhere: a built-in PCM WAV reader. Runs on a background thread, never the audio
// or EE thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace music {

enum class DecodeStatus { Ok, Protected, Unsupported, Failed };
struct DecodeResult {
    DecodeStatus status = DecodeStatus::Failed;
    std::string reason;  // shown in the MODS tab
};
// Called with blocks of interleaved stereo frames; return false to cancel.
using DecodeSink = std::function<bool(const int16_t *frames, size_t count)>;

DecodeResult decode_file(const std::string &path, const DecodeSink &sink);
// Lower-case extension (with dot) a music file may have; others in the folder are ignored.
bool is_music_extension(const std::string &ext);

}  // namespace music
