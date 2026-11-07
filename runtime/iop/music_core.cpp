#include "music_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace music {

std::vector<BankRecord> parse_bank_music(const uint8_t *b, size_t n) {
    std::vector<BankRecord> out;
    if (n < 16 || memcmp(b, "knaB", 4) != 0) return out;
    uint32_t count;
    memcpy(&count, b + 8, 4);
    for (uint32_t i = 0; i < count && 16 + (size_t)(i + 1) * 12 <= n; i++) {
        const uint8_t *r = b + 16 + (size_t)i * 12;
        BankRecord rec;
        memcpy(&rec.id, r, 2);
        memcpy(&rec.start_sector, r + 4, 4);
        memcpy(&rec.bytes, r + 8, 4);
        if (rec.bytes >= kMusicMinBytes) out.push_back(rec);
    }
    return out;
}

const BankRecord *find_music_record(const std::vector<BankRecord> &recs, uint32_t bank_lsn, uint32_t lsn, uint32_t bytes) {
    for (const BankRecord &r : recs)
        if (bank_lsn + r.start_sector == lsn && r.bytes == bytes) return &r;
    return nullptr;
}

uint64_t ShuffleBag::rand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
}

uint32_t ShuffleBag::next(const std::vector<uint32_t> &custom, int native_slots) {
    if (custom.empty()) return 0;
    if (bag_.empty() || set_ != custom) {
        set_ = custom;
        bag_ = custom;
        for (int i = 0; i < native_slots; i++) bag_.push_back(0);
        for (size_t i = bag_.size() - 1; i > 0; i--) std::swap(bag_[i], bag_[rand() % (i + 1)]);
    }
    uint32_t v = bag_.back();
    bag_.pop_back();
    return v;
}

void LoudnessMeter::add(const int16_t *s, size_t frames) {
    constexpr size_t kBlock = 19200;  // 0.4 s
    for (size_t i = 0; i < frames; i++) {
        const int l = s[i * 2], r = s[i * 2 + 1];
        acc_ += (double)l * l + (double)r * r;
        peak_ = std::max(peak_, (float)std::max(std::abs(l), std::abs(r)));
        if (++in_block_ == kBlock) {
            blocks_.push_back(acc_ / (kBlock * 2.0));
            acc_ = 0;
            in_block_ = 0;
        }
    }
}

float LoudnessMeter::rms() const {
    std::vector<double> b = blocks_;
    if (b.empty() && in_block_ > 0) b.push_back(acc_ / (in_block_ * 2.0));  // very short clip
    const double abs_gate = std::pow(32768.0 * std::pow(10.0, -50.0 / 20.0), 2);
    double sum = 0;
    size_t n = 0;
    for (double v : b)
        if (v > abs_gate) { sum += v; n++; }
    if (!n) return 0;
    const double rel_gate = sum / n * 0.1;  // -10 dB below the first-pass mean
    double sum2 = 0;
    size_t n2 = 0;
    for (double v : b)
        if (v > abs_gate && v > rel_gate) { sum2 += v; n2++; }
    return n2 ? (float)std::sqrt(sum2 / n2) : 0.0f;
}

float LoudnessMeter::gain() const {
    const float r = rms();
    if (r <= 0 || peak_ <= 0) return 1.0f;
    float g = std::clamp(kTargetRms / r, 0.25f, 4.0f);
    return std::min(g, 32700.0f / peak_);  // never clip
}

std::string state_to_json(const std::vector<std::string> &disabled) {
    std::string o = "{\n  \"version\": 1,\n  \"disabled\": [";
    for (size_t i = 0; i < disabled.size(); i++) {
        o += i ? ",\n    \"" : "\n    \"";
        for (char c : disabled[i]) {
            if (c == '"' || c == '\\') o += '\\';
            if ((unsigned char)c < 0x20) continue;
            o += c;
        }
        o += '"';
    }
    o += disabled.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

std::vector<std::string> state_from_json(const std::string &t) {
    std::vector<std::string> out;
    size_t at = t.find("\"disabled\"");
    if (at == std::string::npos) return out;
    at = t.find('[', at);
    if (at == std::string::npos) return out;
    for (size_t i = at + 1; i < t.size() && t[i] != ']'; i++) {
        if (t[i] != '"') continue;
        std::string s;
        for (i++; i < t.size() && t[i] != '"'; i++) {
            if (t[i] == '\\' && i + 1 < t.size()) i++;
            s += t[i];
        }
        out.push_back(s);
    }
    return out;
}

uint64_t fnv1a(const std::string &s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

bool HostVoice::next(int &l, int &r) {
    l = r = 0;
    if (!active || !pcm) return false;
    if (pos >= frames) { active = false; return false; }
    l = (pcm[pos * 2] * gain_q12) >> 12;
    r = (pcm[pos * 2 + 1] * gain_q12) >> 12;
    l = std::clamp(l, -32768, 32767);
    r = std::clamp(r, -32768, 32767);
    pos++;
    return true;
}

}  // namespace music
