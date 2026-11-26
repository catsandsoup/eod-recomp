#pragma once

#include "netplay_map_transfer.h"
#include "../runtime/vfs/custom_map.h"
#include "../runtime/app/game_frontend.h"

namespace netplay::maps {
class LobbyMaps {
public:
    Offer offer;
    std::string id, verified, base_arena;
    std::vector<game_frontend::CustomMapOption> catalog;
    Download download;
    uint64_t next_request = 0, last_progress = 0;
    uint32_t last_window = 0;

    void inventory(const fs::path &directory) {
        cache_ = directory;
        catalog.clear();
        std::error_code ec;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            const auto &entry = *it;
            if (entry.path().extension() != ".eodmap" || !fs::is_regular_file(entry.symlink_status(ec))) continue;
            std::string error;
            if (auto map = eod::vfs::inspect_map(entry.path(), error))
                catalog.push_back({entry.path().filename().u8string(), map->title, map->base_arena, ""});
        }
        std::sort(catalog.begin(), catalog.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    }

    bool select(const std::string &filename, std::string &error) {
        if (filename.empty()) { clear(); return true; }
        auto item = std::find_if(catalog.begin(), catalog.end(), [&](const auto &o) { return o.id == filename; });
        if (item == catalog.end()) { error = "Choose an installed map"; return false; }
        auto file = cache_ / fs::u8path(item->id);
        std::error_code ec;
        auto size = fs::file_size(file, ec);
        if (ec || !size || size > archive_limit || !fs::is_regular_file(fs::symlink_status(file, ec))) {
            error = "Map archive is missing or too large"; return false;
        }
        Offer choice{eod::vfs::map_archive_sha256(file, error), item->title.substr(0, 160), uint32_t(size)};
        if (!choice.valid()) { if (error.empty()) error = "Invalid map metadata"; return false; }
        std::ifstream source(file, std::ios::binary);
        if (!source) { error = "Cannot open map archive"; return false; }
        if (!eod::vfs::mount_map(file, eod::vfs::Layer::ServerSession, error, choice.hash)) return false;
        source_ = std::move(source);
        id = filename;
        record_mount(choice.hash);
        offer = std::move(choice);
        return true;
    }

    bool receive_offer(const fs::path &directory, const Offer &remote, std::string &error) {
        if (!remote.valid()) { error = "Invalid host map"; return false; }
        if (remote.hash == offer.hash) return remote.size == offer.size;
        clear();
        if (remote.hash.empty()) return true;
        cache_ = directory;
        offer = remote;
        id = remote.hash + ".eodmap";
        auto cached = cache_ / id;
        std::error_code ec;
        if (fs::is_regular_file(fs::symlink_status(cached, ec)) &&
            eod::vfs::mount_map(cached, eod::vfs::Layer::ServerSession, error, remote.hash)) {
            record_mount(remote.hash);
            return true;
        }
        // Installed maps can have user-chosen names. A matching file size avoids
        // hashing unrelated archives. Every mount still checks the exact file SHA.
        ec.clear();
        for (fs::directory_iterator it(cache_, ec), end; !ec && it != end; it.increment(ec)) {
            auto &file = *it;
            if (file.path() == cached || file.path().extension() != ".eodmap" ||
                !fs::is_regular_file(file.symlink_status(ec)) || file.file_size(ec) != remote.size) continue;
            if (eod::vfs::mount_map(file.path(), eod::vfs::Layer::ServerSession, error, remote.hash)) {
                record_mount(remote.hash);
                return true;
            }
        }
        error.clear();
        return download.open(cache_, remote, error);
    }

    bool read_chunk(uint32_t index, std::vector<uint8_t> &bytes) {
        const auto length = offer.length(index);
        if (!length || !source_.is_open()) return false;
        bytes.resize(length);
        source_.clear();
        source_.seekg(std::streamoff(index) * chunk_bytes);
        return bool(source_.read(reinterpret_cast<char *>(bytes.data()), length));
    }

    bool finish_download(std::string &error) {
        if (!download.complete()) { error = "Incomplete map download"; return false; }
        const auto staged = download.path();
        // Verify both the wire digest and all allowed package members before publishing.
        if (!eod::vfs::mount_map(staged, eod::vfs::Layer::ServerSession, error, offer.hash)) return false;
        const auto destination = cache_ / (offer.hash + ".eodmap");
        std::error_code ec;
        fs::rename(staged, destination, ec);
        if (ec && eod::vfs::map_archive_sha256(destination, error) != offer.hash) {
            eod::vfs::global_vfs().unmount(eod::vfs::Layer::ServerSession);
            error = "Could not cache verified map: " + ec.message();
            return false;
        }
        download.clear();
        record_mount(offer.hash);
        return true;
    }

    void clear() {
        if (!verified.empty()) eod::vfs::global_vfs().unmount(eod::vfs::Layer::ServerSession);
        download.clear();
        source_.close();
        source_.clear();
        offer = {};
        id.clear(); verified.clear(); base_arena.clear();
        next_request = last_progress = 0;
        last_window = 0;
    }
private:
    void record_mount(const std::string &hash) {
        verified = hash;
        base_arena = eod::vfs::active_map()->base_arena;
    }

    fs::path cache_;
    std::ifstream source_;
};
}
