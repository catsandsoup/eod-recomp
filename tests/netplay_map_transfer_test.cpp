#include "../src/netplay_lobby_maps.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
    using namespace netplay::maps;
    if (argc != 2) return 2;
    try {
        const fs::path root = fs::u8path(argv[1]);
        LobbyMaps host, guest;
        std::string error;
        host.inventory(root / "host/mods/maps");
        require(host.catalog.size() == 1 && host.catalog.front().title == "Banked test", "host catalog");
        require(!host.select("../unknown.eodmap", error), "unknown map ID");
        require(host.select("track.eodmap", error), error);
        const Offer offer = host.offer;
        require(host.verified == offer.hash && host.base_arena == "AFTON", "host mount");
        auto deliver = [&](uint32_t index, bool corrupt = false) {
            std::vector<uint8_t> bytes;
            require(host.read_chunk(index, bytes), "host chunk");
            if (corrupt) bytes.back() ^= 1;
            return guest.download.accept(index, bytes.data(), bytes.size(), error);
        };
        auto cache = root / "client/mods/maps";
        require(guest.receive_offer(cache, offer, error), error);
        require(guest.verified.empty(), "must verify before ready");
        for (uint32_t i = window_chunks; i-- > 1;) require(deliver(i), "out-of-order chunk");
        require(!deliver(4), "duplicate chunk");
        auto missing = guest.download.window();
        require(missing.first == 0 && missing.missing == 1, "retransmit only missing chunk");
        require(deliver(0), "retransmission");
        while (!guest.download.complete()) require(deliver(guest.download.window().first), "finish transfer");
        require(guest.finish_download(error), error);
        require(guest.verified == offer.hash && guest.base_arena == host.base_arena && eod::vfs::active_map()->title == "Banked test", "guest mount");
        require(fs::is_regular_file(cache / (offer.hash + ".eodmap")), "verified map cache");
        guest.clear();
        require(!eod::vfs::active_map(), "disconnect unmount");
        require(guest.receive_offer(cache, offer, error) && guest.verified == offer.hash && guest.base_arena == host.base_arena, "reuse cache");
        require(guest.receive_offer(cache, {}, error) && !eod::vfs::active_map(), "switch to retail");
        const auto bad_cache = root / "corrupt/mods/maps";
        require(guest.receive_offer(bad_cache, offer, error), error);
        for (uint32_t i = 0; i < offer.chunks(); ++i) require(deliver(i, i == 0), "corrupt transfer");
        require(!guest.finish_download(error) && guest.verified.empty() && !eod::vfs::active_map(), "reject bad digest");
        require(!fs::exists(bad_cache / (offer.hash + ".eodmap")), "unverified cache publication");
        guest.clear();
        require(fs::is_empty(bad_cache), "partial cleanup");
        require(host.select("", error) && host.offer.hash.empty(), "host reset");
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
