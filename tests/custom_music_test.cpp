// Custom music tests: decoder wrapper (a generated WAV), shuffle ID mapping, mixer
// substitution and the library scan/state, without the game.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "core/log.h"
#include "iop/custom_music.h"
#include "iop/music_core.h"
#include "iop/music_blocks.h"
#include "iop/music_decode.h"

bool g_log_enabled[LOG_CAT_COUNT];
void log_write(LogCat, const char *, ...) {}
std::string mac_app_support() { return "."; }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace fs = std::filesystem;

// 16-bit PCM WAV, `secs` seconds of a sine, stereo or mono
static void write_wav(const fs::path &p, int rate, int ch, double secs, double hz, double amp) {
    const uint32_t frames = (uint32_t)(rate * secs), bytes = frames * ch * 2;
    std::ofstream o(p, std::ios::binary);
    auto w32 = [&](uint32_t v) { o.write((const char *)&v, 4); };
    auto w16 = [&](uint16_t v) { o.write((const char *)&v, 2); };
    o.write("RIFF", 4); w32(36 + bytes); o.write("WAVEfmt ", 8); w32(16); w16(1); w16((uint16_t)ch); w32((uint32_t)rate);
    w32((uint32_t)(rate * ch * 2)); w16((uint16_t)(ch * 2)); w16(16); o.write("data", 4); w32(bytes);
    for (uint32_t i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) {
            int16_t v = (int16_t)(std::sin(2 * M_PI * hz * i / rate) * amp * 32767);
            o.write((const char *)&v, 2);
        }
}

int main() {
    { // Songs and speech share stream rings; stale music tags must not mute speech.
        using namespace music_blocks;
        reset();
        set(first, true);set(end - 16, true);
        CHECK(contains(first) && contains(first + 15) && contains(end - 1));
        CHECK(!contains(first - 1) && !contains(end));
        clear(first + 15, 2); // A partial write covers two ADPCM blocks.
        CHECK(!contains(first) && contains(end - 16));
        set(first + 16, true);set(first + 32, true);
        clear(first + 16, 16);
        CHECK(!contains(first + 16) && contains(first + 32));
        clear(end - 1, 2); // Wrapped RAM write clears the final block.
        CHECK(!contains(end - 16) && contains(first + 32));
        reset();CHECK(!contains(first + 32));
    }

    const fs::path dir = fs::temp_directory_path() / "eod_custom_music_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "lib" / "sub");

    // ---- decoder wrapper: 44.1 kHz mono WAV -> 48 kHz stereo
    write_wav(dir / "lib" / "tone.wav", 44100, 1, 1.0, 440, 0.5);
    {
        std::vector<int16_t> pcm;
        music::DecodeResult r = music::decode_file((dir / "lib" / "tone.wav").string(), [&](const int16_t *s, size_t n) {
            pcm.insert(pcm.end(), s, s + n * 2);
            return true;
        });
        CHECK(r.status == music::DecodeStatus::Ok);
        const size_t frames = pcm.size() / 2;
        CHECK(frames > 47000 && frames < 49000);  // 1 s at 48 kHz
        CHECK(pcm[2000] == pcm[2001]);            // mono -> both channels
        music::LoudnessMeter m;
        m.add(pcm.data(), frames);
        CHECK(std::fabs(m.rms() - 0.5 * 32767 / std::sqrt(2.0)) < 600);
        CHECK(m.gain() >= 0.25f && m.peak() * m.gain() <= 32767.0f);
    }
    {  // garbage and protected files are reported, not thrown
        std::ofstream(dir / "lib" / "bad.mp3") << "this is not audio";
        music::DecodeResult r = music::decode_file((dir / "lib" / "bad.mp3").string(), [](const int16_t *, size_t) { return true; });
        CHECK(r.status != music::DecodeStatus::Ok && !r.reason.empty());
        std::ofstream(dir / "lib" / "sub" / "bought.m4p") << "x";
        r = music::decode_file((dir / "lib" / "sub" / "bought.m4p").string(), [](const int16_t *, size_t) { return true; });
        CHECK(r.status == music::DecodeStatus::Protected);
    }

    // ---- shuffle ID mapping: bank records, queue request -> record, bag
    {
        std::vector<uint8_t> bank(16 + 12 * 4, 0);
        memcpy(bank.data(), "knaB", 4);
        const uint32_t ver = 1, count = 4;
        memcpy(&bank[4], &ver, 4); memcpy(&bank[8], &count, 4);
        const uint32_t rec[4][3] = {{5, 100, 2086144}, {7, 900, 2000}, {25, 2000, 914624}, {9, 4000, 2027264}};
        for (int i = 0; i < 4; i++) {
            uint16_t id = (uint16_t)rec[i][0];
            memcpy(&bank[16 + i * 12], &id, 2);
            memcpy(&bank[16 + i * 12 + 4], &rec[i][1], 4);
            memcpy(&bank[16 + i * 12 + 8], &rec[i][2], 4);
        }
        auto recs = music::parse_bank_music(bank.data(), bank.size());
        CHECK(recs.size() == 3);  // the 2000-byte speech cue is not music
        const uint32_t bank_lsn = 77000;
        const music::BankRecord *r = music::find_music_record(recs, bank_lsn, bank_lsn + 4000, 2027264);
        CHECK(r && r->id == 9);
        CHECK(!music::find_music_record(recs, bank_lsn, bank_lsn + 900, 2000));
        CHECK(!music::find_music_record(recs, bank_lsn, bank_lsn + 4000, 1));
        CHECK(music::is_virtual_id(0x8000) && !music::is_virtual_id(25));

        music::ShuffleBag bag(42);
        CHECK(bag.next({}, 12) == 0);  // no custom songs: the game's pick stands
        const std::vector<uint32_t> ids = {0x8000, 0x8001, 0x8002};
        for (int cycle = 0; cycle < 3; cycle++) {  // each cycle: every custom song once + 12 native slots
            int native = 0, seen[3] = {};
            for (int i = 0; i < 15; i++) {
                uint32_t v = bag.next(ids, 12);
                if (v == 0) native++; else { CHECK(v >= 0x8000 && v < 0x8003); seen[v - 0x8000]++; }
            }
            CHECK(native == 12 && seen[0] == 1 && seen[1] == 1 && seen[2] == 1);
        }
    }

    // ---- state.json round trip (quotes, backslashes, unicode)
    {
        std::vector<std::string> d = {"MX Unleashed/3against1 - It's Over.mp3", "a \"q\" b.mp3", "caf\xC3\xA9.flac"};
        CHECK(music::state_from_json(music::state_to_json(d)) == d);
        CHECK(music::state_from_json("{}").empty());
    }

    // ---- mixer substitution: host PCM through the voice's envelope and volumes
    {
        std::vector<int16_t> pcm = {1000, -2000, 3000, -4000};  // two stereo frames
        music::HostVoice v;
        v.pcm = pcm.data(); v.frames = 2; v.active = true; v.gain_q12 = 8192;  // x2
        int l, r;
        CHECK(v.next(l, r) && l == 2000 && r == -4000);
        int32_t dl = 0, dr = 0;
        music::host_voice_mix(l, r, 0x7FFF, 0x4000, 0x2000, true, true, dl, dr);  // vol 0.5 / 0.25
        CHECK(std::abs(dl - 1000) <= 2 && std::abs(dr + 1000) <= 2);
        dl = dr = 0;
        music::host_voice_mix(l, r, 0x7FFF, 0x4000, 0x2000, false, true, dl, dr);  // left mix bit off
        CHECK(dl == 0 && std::abs(dr + 1000) <= 2);
        dl = dr = 0;
        music::host_voice_mix(l, r, 0, 0x4000, 0x4000, true, true, dl, dr);  // envelope closed (key off / ducked to 0)
        CHECK(dl == 0 && dr == 0);
        CHECK(v.next(l, r) && l == 6000 && !v.finished());
        CHECK(!v.next(l, r) && v.finished() && l == 0);
    }

    // ---- library: scan, skip reasons, per-song state, host playback from the cache
    {
        write_wav(dir / "lib" / "sub" / "quiet.wav", 48000, 2, 2.0, 220, 0.05);
        const std::string root = (dir / "lib").string(), cache = (dir / "cache").string();
        custom_music::init(root, cache);
        for (int i = 0; i < 400 && (custom_music::progress().scanning || custom_music::songs().empty()); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        auto songs = custom_music::songs();
        CHECK(songs.size() == 4);  // bad.mp3, bought.m4p, quiet.wav, tone.wav
        int ready = 0, skipped = 0, prot = 0;
        for (auto &s : songs) {
            ready += s.state == custom_music::State::Ready;
            skipped += s.state == custom_music::State::Skipped;
            prot += s.state == custom_music::State::Protected;
            if (s.state != custom_music::State::Ready) CHECK(!s.reason.empty());
        }
        for (auto &s : songs) std::fprintf(stderr, "%s state=%d %s\n", s.rel.c_str(), (int)s.state, s.reason.c_str());
        CHECK(ready == 2 && skipped == 1 && prot == 1);
        for (auto &s : songs)
            if (s.rel == "sub/quiet.wav") CHECK(s.gain > 2.0f);  // a quiet song is raised toward the native level
        CHECK(custom_music::ready_count() == 2);
        uint32_t tone = 0;
        for (auto &s : songs) if (s.rel == "tone.wav") tone = s.id;
        CHECK(tone >= 0x8000);
        // pick: with both enabled, a custom id comes up within one cycle of 14
        bool got = false;
        for (int i = 0; i < 14; i++) got |= custom_music::pick_for_native() != 0;
        CHECK(got);
        custom_music::set_enabled("sub/quiet.wav", false);
        CHECK(custom_music::ready_count() == 1);
        CHECK(fs::exists(dir / "lib" / "state.json"));
        // host voice: preload, start, mix frames until the end
        custom_music::host_preload(0, tone);
        custom_music::host_start(0);
        size_t played = 0;
        for (int i = 0; i < 400 && !custom_music::host_active(0); i++) {
            custom_music::host_begin_block();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(custom_music::host_active(0));
        int l, r;
        while (custom_music::host_frame(0, l, r)) played++;
        CHECK(played > 47000 && custom_music::host_finished(0));
        custom_music::host_stop(0);
        CHECK(!custom_music::host_active(0));
        custom_music::host_preload(1, 0x9999);  // unknown song: finishes as failed, never stalls
        for (int i = 0; i < 400 && !custom_music::host_finished(1); i++) {
            custom_music::host_begin_block();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(custom_music::host_finished(1));
        custom_music::shutdown();
    }
    fs::remove_all(dir);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("custom_music_test: ok");
    return 0;
}
