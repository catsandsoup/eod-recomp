#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace netplay::maps {
namespace fs = std::filesystem;
constexpr uint32_t chunk_bytes = 1024;
constexpr uint32_t archive_limit = 256u << 20;
constexpr uint32_t window_chunks = 32;

struct Offer {
    std::string hash, title;
    uint32_t size = 0;

    bool valid() const {
        if (hash.empty()) return title.empty() && size == 0;
        return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == hash.npos &&
               !title.empty() && title.size() <= 160 && size > 0 && size <= archive_limit;
    }
    uint32_t chunks() const { return (size + chunk_bytes - 1) / chunk_bytes; }
    uint32_t length(uint32_t index) const {
        return index < chunks() ? std::min(chunk_bytes, size - index * chunk_bytes) : 0;
    }
};

struct Window { uint32_t first = 0, missing = 0; };

// Each request identifies holes in the current window. Duplicate/reordered UDP
// replies are harmless; a lost request or reply is retried by the next poll.
class Download {
public:
    Download() = default;
    Download(const Download &) = delete;
    Download &operator=(const Download &) = delete;
    ~Download() { clear(); }

    bool open(const fs::path &cache, const Offer &offer, std::string &error) {
        clear();
        if (!offer.valid() || !offer.size) { error = "Invalid map offer"; return false; }
        std::error_code ec;
        fs::create_directories(cache, ec);
        if (ec) { error = ec.message(); return false; }
        fs::path dir = cache / (".incoming-" + std::to_string(std::random_device{}()) +
                                "-" + std::to_string(std::random_device{}()));
        if (!fs::create_directory(dir, ec)) { error = "Cannot create map download folder"; return false; }
        stage_ = dir;
        file_.open(path(), std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file_) { error = "Cannot stage map download"; clear(); return false; }
        offer_ = offer;
        received_.assign(offer.chunks(), false);
        return true;
    }
    fs::path path() const { return stage_ / "download.eodmap"; }
    bool complete() const { return offer_.size && first_ == offer_.chunks(); }
    uint32_t received_bytes() const { return bytes_; }
    Window window() const {
        Window result{first_, 0};
        for (uint32_t i = 0; i < window_chunks && first_ + i < received_.size(); ++i)
            if (!received_[first_ + i]) result.missing |= uint32_t(1) << i;
        return result;
    }
    bool accept(uint32_t index, const uint8_t *data, size_t length, std::string &error) {
        if (index < first_ || index - first_ >= window_chunks || !length ||
            length != offer_.length(index) || received_[index]) return false;
        file_.seekp(std::streamoff(index) * chunk_bytes);
        file_.write(reinterpret_cast<const char *>(data), length);
        if (!file_) { error = "Failed writing map download"; return false; }
        received_[index] = true;
        bytes_ += static_cast<uint32_t>(length);
        while (first_ < received_.size() && received_[first_]) ++first_;
        if (complete()) {
            file_.close();
            if (!file_) { error = "Failed closing map download"; return false; }
        }
        return true;
    }
    void clear() {
        if (file_.is_open()) file_.close();
        file_.clear();
        if (!stage_.empty()) {
            std::error_code ec;
            fs::remove(path(), ec);
            fs::remove(stage_, ec);
            stage_.clear();
        }
        offer_ = {};
        first_ = bytes_ = 0;
        received_.clear();
    }
private:
    fs::path stage_;
    Offer offer_;
    std::ofstream file_;
    std::vector<bool> received_;
    uint32_t first_ = 0, bytes_ = 0;
};
}
