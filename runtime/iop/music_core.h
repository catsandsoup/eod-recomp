// music_core.h - pure parts of the custom music feature (no threads, no OS, no game
// state): SPEECH.BNK music records, the shuffle that mixes custom songs in with the
// native ones, loudness measurement, the state.json format, the cache header and the
// host-PCM voice used by the SPU2 mixer. Unit-tested by tests/custom_music_test.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace music {

constexpr uint32_t kVirtualBase = 0x8000;  // virtual song ids: kVirtualBase + index
inline bool is_virtual_id(uint32_t id) { return id >= kVirtualBase; }

// ---- SPEECH.BNK: header 'knaB', u32 version, u32 count, then 12-byte records at 0x10
// {u16 id, u16 pad, u32 start_sector (2048 B, relative to the bank file), u32 size}.
// The 13 retail songs are the records larger than kMusicMinBytes.
constexpr uint32_t kMusicMinBytes = 300000;
struct BankRecord {
    uint16_t id;
    uint32_t start_sector, bytes;
};
// Parses the music records out of the first `n` bytes of a bank (header + table).
std::vector<BankRecord> parse_bank_music(const uint8_t *bank, size_t n);
// The stream request {lsn, bytes} the EE queued for a bank at `bank_lsn`: the music
// record it names, or nullptr (speech cue, effect, other data).
const BankRecord *find_music_record(const std::vector<BankRecord> &recs, uint32_t bank_lsn, uint32_t lsn, uint32_t bytes);

// ---- Shuffle. The native game picks a song at random from its own 12-song table; the
// bag decides, for each song the game queues, whether it plays as the game chose
// (returns 0) or is replaced by a custom song (returns its virtual id). One cycle holds
// every enabled custom song once plus `native_slots` native slots, so custom and native
// songs share the rotation evenly and nothing repeats before the cycle ends. Uses a
// private RNG: the simulation's random streams are never touched.
class ShuffleBag {
public:
    explicit ShuffleBag(uint64_t seed = 0x9E3779B97F4A7C15ull) : rng_(seed ? seed : 1) {}
    uint32_t next(const std::vector<uint32_t> &custom_ids, int native_slots = 12);
private:
    uint64_t rand();
    uint64_t rng_;
    std::vector<uint32_t> bag_, set_;
};

// ---- Loudness. Mean level (RMS, 16-bit units, per channel) over gated 0.4 s blocks, so
// silence does not drag a song's level down; the gain brings it to the native songs'
// average (9000, measured from the 13 decoded retail songs) without clipping.
constexpr float kTargetRms = 9000.0f;
class LoudnessMeter {
public:
    void add(const int16_t *stereo, size_t frames);
    float rms() const;       // gated RMS, 0 if silent
    float peak() const { return peak_; }
    float gain() const;      // linear gain, clamped to [0.25, 4] and to avoid clipping
private:
    std::vector<double> blocks_;
    double acc_ = 0;
    size_t in_block_ = 0;
    float peak_ = 0;
};

// ---- state.json: {"version":1,"disabled":["relative/path.mp3", ...]}
std::string state_to_json(const std::vector<std::string> &disabled);
std::vector<std::string> state_from_json(const std::string &text);

// ---- Decoded-song cache file: this header, then frames * 2 int16 (48 kHz stereo).
struct CacheHeader {
    char magic[4];       // 'T','D','M','1'
    uint32_t reserved;
    uint64_t frames;
    float gain, rms;
};
inline bool cache_header_ok(const CacheHeader &h) {
    return h.magic[0] == 'T' && h.magic[1] == 'D' && h.magic[2] == 'M' && h.magic[3] == '1' && h.frames > 0;
}
uint64_t fnv1a(const std::string &s);

// ---- Host-PCM voice (what the mixer plays in place of a stream voice's ADPCM).
struct HostVoice {
    const int16_t *pcm = nullptr;  // interleaved stereo
    size_t frames = 0, pos = 0;
    int gain_q12 = 4096;           // loudness match
    bool active = false;
    bool next(int &l, int &r);     // false once the song has ended (or nothing to play)
    bool finished() const { return !active && pcm != nullptr && pos >= frames; }
};

// The mixer's per-voice contribution of a host voice: the voice's envelope level and
// left/right volumes exactly as for an ADPCM voice (so the game's music volume, ducking
// and stop/start apply). `dl`/`dr` are added to when the matching mix bit is set.
inline void host_voice_mix(int hl, int hr, int env_level, int vol_l, int vol_r, bool mix_l, bool mix_r,
                           int32_t &dl, int32_t &dr) {
    hl = (hl * env_level) >> 15;
    hr = (hr * env_level) >> 15;
    if (mix_l) dl += (hl * vol_l) >> 15;
    if (mix_r) dr += (hr * vol_r) >> 15;
}

}  // namespace music
