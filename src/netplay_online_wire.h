#pragma once
// Garage Code internet layer, pure codecs (no sockets): SHA-1, join codes, rendezvous hashes,
// STUN, BitTorrent UDP tracker (BEP 15), NAT-PMP / PCP and UPnP IGD messages.
// Everything here is header-only and unit tested (tests/netplay_online_test.cpp).
// Design: docs/NETPLAY_INTERNET.md.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace netplay_online {

// ---------------------------------------------------------------------------- SHA-1
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t buf[64];
    uint64_t total = 0;
    size_t used = 0;
    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void block(const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void update(const void *data, size_t n) {
        const uint8_t *p = (const uint8_t *)data;
        total += n;
        while (n) {
            const size_t take = n < 64 - used ? n : 64 - used;
            std::memcpy(buf + used, p, take);
            used += take; p += take; n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    void final(uint8_t out[20]) {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 5; ++i) { out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16); out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i]; }
    }
};
inline std::vector<uint8_t> sha1(const std::string &s) {
    Sha1 c; c.update(s.data(), s.size());
    std::vector<uint8_t> out(20); c.final(out.data());
    return out;
}
inline std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
inline std::string hex(const std::vector<uint8_t> &v) { return hex(v.data(), v.size()); }
inline bool unhex(const std::string &s, std::vector<uint8_t> &out) {
    out.clear();
    if (s.size() % 2) return false;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i += 2) { int a = nib(s[i]), b = nib(s[i + 1]); if (a < 0 || b < 0) return false; out.push_back((uint8_t)(a << 4 | b)); }
    return true;
}

// ---------------------------------------------------------------------------- codes
// 5 characters from 31 letters/digits without 0 O 1 I L (28.6 million values).
inline const char *code_alphabet() { return "23456789ABCDEFGHJKMNPQRSTUVWXYZ"; }
enum class CodeCheck { Ok, TooShort, TooLong, BadChar };
// Accepts what a person types or pastes: any case, spaces and dashes ignored.
inline CodeCheck normalize_code(const std::string &in, std::string &out) {
    out.clear();
    const char *alpha = code_alphabet();
    for (char c : in) {
        if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!std::strchr(alpha, c) || c == 0) return CodeCheck::BadChar;
        out.push_back(c);
    }
    if (out.size() < 5) return CodeCheck::TooShort;
    if (out.size() > 5) return CodeCheck::TooLong;
    return CodeCheck::Ok;
}
inline std::string code_from_bytes(const uint8_t *r, size_t n) {   // n >= 5 random bytes
    std::string c;
    for (size_t i = 0; i < 5 && i < n; ++i) c.push_back(code_alphabet()[r[i] % 31]);
    return c;
}
// Rendezvous keys. `secret` is the join code ("K7QD2") or a remembered-friend secret (hex text).
// kind: 'H' = the host announces here, 'J' = a joiner announces here, 'T' = join tag.
inline std::vector<uint8_t> rendezvous_hash(char kind, const std::string &secret) {
    return sha1(std::string("TDEOD-EVE-NET-1/") + kind + "/" + secret);
}
inline uint32_t join_tag(const std::string &secret) {
    auto h = rendezvous_hash('T', secret);
    return (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
}

// ---------------------------------------------------------------------------- IPv4 helpers
inline bool is_private_or_shared_ipv4(uint32_t ip) {   // host order
    const uint32_t a = ip >> 24, b = (ip >> 16) & 255;
    return a == 10 || a == 127 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) ||
           (a == 100 && b >= 64 && b <= 127) || a >= 224;
}
inline std::string ip_text(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

// ---------------------------------------------------------------------------- STUN (RFC 5389)
constexpr uint32_t kStunCookie = 0x2112A442u;
inline void put16(std::vector<uint8_t> &b, uint32_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
inline void put32(std::vector<uint8_t> &b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
inline void put64(std::vector<uint8_t> &b, uint64_t v) { put32(b, (uint32_t)(v >> 32)); put32(b, (uint32_t)v); }
inline uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
inline uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }
inline uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

inline std::vector<uint8_t> stun_request(const uint8_t tid[12]) {
    std::vector<uint8_t> b;
    put16(b, 0x0001); put16(b, 0); put32(b, kStunCookie);
    b.insert(b.end(), tid, tid + 12);
    return b;
}
// A datagram is STUN when it has the 20-byte header, top two bits zero and the magic cookie.
inline bool looks_like_stun(const uint8_t *d, size_t n) { return n >= 20 && (d[0] & 0xC0) == 0 && get32(d + 4) == kStunCookie; }
inline bool stun_parse_response(const uint8_t *d, size_t n, const uint8_t tid[12], uint32_t &ip, uint16_t &port) {
    if (!looks_like_stun(d, n) || get16(d) != 0x0101 || std::memcmp(d + 8, tid, 12) != 0) return false;
    const size_t len = get16(d + 2);
    if (20 + len > n) return false;
    size_t o = 20;
    bool got = false;
    while (o + 4 <= 20 + len) {
        const uint32_t type = get16(d + o), alen = get16(d + o + 2);
        if (o + 4 + alen > 20 + len) break;
        const uint8_t *v = d + o + 4;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            if (type == 0x0020) { port = (uint16_t)(get16(v + 2) ^ (kStunCookie >> 16)); ip = get32(v + 4) ^ kStunCookie; got = true; }
            else if (!got) { port = (uint16_t)get16(v + 2); ip = get32(v + 4); got = true; }
        }
        o += 4 + ((alen + 3) & ~3u);
    }
    return got;
}

// ---------------------------------------------------------------------------- UDP tracker (BEP 15)
constexpr uint64_t kTrackerProtocolId = 0x41727101980ull;
enum TrackerEvent : uint32_t { TE_NONE = 0, TE_COMPLETED = 1, TE_STARTED = 2, TE_STOPPED = 3 };
inline std::vector<uint8_t> tracker_connect(uint32_t tid) {
    std::vector<uint8_t> b; put64(b, kTrackerProtocolId); put32(b, 0); put32(b, tid); return b;
}
inline bool tracker_parse_connect(const uint8_t *d, size_t n, uint32_t &tid, uint64_t &cid) {
    if (n < 16 || get32(d) != 0) return false;
    tid = get32(d + 4); cid = get64(d + 8); return true;
}
inline std::vector<uint8_t> tracker_announce(uint64_t cid, uint32_t tid, const std::vector<uint8_t> &info_hash, const uint8_t peer_id[20],
                                              uint64_t left, TrackerEvent ev, uint16_t port, int32_t want) {
    std::vector<uint8_t> b;
    put64(b, cid); put32(b, 1); put32(b, tid);
    b.insert(b.end(), info_hash.begin(), info_hash.end());
    b.insert(b.end(), peer_id, peer_id + 20);
    put64(b, 0); put64(b, left); put64(b, 0);
    put32(b, ev); put32(b, 0); put32(b, 0xC0DEu); put32(b, (uint32_t)want); put16(b, port);
    return b;
}
struct TrackerPeer { uint32_t ip; uint16_t port; };
inline bool tracker_parse_announce(const uint8_t *d, size_t n, uint32_t &tid, uint32_t &interval, std::vector<TrackerPeer> &peers) {
    peers.clear();
    if (n < 20 || get32(d) != 1) return false;
    tid = get32(d + 4); interval = get32(d + 8);
    for (size_t o = 20; o + 6 <= n; o += 6) peers.push_back({get32(d + o), (uint16_t)get16(d + o + 4)});
    return true;
}
inline bool tracker_is_error(const uint8_t *d, size_t n, uint32_t &tid) {
    if (n < 8 || get32(d) != 3) return false;
    tid = get32(d + 4); return true;
}

// ---------------------------------------------------------------------------- NAT-PMP (RFC 6886) and PCP (RFC 6887)
inline std::vector<uint8_t> pmp_external_request() { return {0, 0}; }
inline bool pmp_parse_external(const uint8_t *d, size_t n, uint32_t &ext_ip) {
    if (n < 12 || d[0] != 0 || d[1] != 128 || get16(d + 2) != 0) return false;
    ext_ip = get32(d + 8); return true;
}
inline std::vector<uint8_t> pmp_map_request(uint16_t internal, uint16_t suggested, uint32_t lifetime) {
    std::vector<uint8_t> b; b.push_back(0); b.push_back(1); put16(b, 0); put16(b, internal); put16(b, suggested); put32(b, lifetime); return b;
}
inline bool pmp_parse_map(const uint8_t *d, size_t n, uint16_t internal, uint16_t &ext_port, uint32_t &lifetime) {
    if (n < 16 || d[0] != 0 || d[1] != 129 || get16(d + 2) != 0 || get16(d + 8) != internal) return false;
    ext_port = (uint16_t)get16(d + 10); lifetime = get32(d + 12); return true;
}
inline void put_mapped_v4(std::vector<uint8_t> &b, uint32_t ip) {   // ::ffff:a.b.c.d
    for (int i = 0; i < 10; ++i) b.push_back(0);
    b.push_back(0xFF); b.push_back(0xFF); put32(b, ip);
}
inline std::vector<uint8_t> pcp_map_request(uint32_t client_ip, const uint8_t nonce[12], uint16_t internal, uint16_t suggested, uint32_t lifetime) {
    std::vector<uint8_t> b;
    b.push_back(2); b.push_back(1); put16(b, 0); put32(b, lifetime);
    put_mapped_v4(b, client_ip);
    b.insert(b.end(), nonce, nonce + 12);
    b.push_back(17); b.push_back(0); b.push_back(0); b.push_back(0);   // UDP
    put16(b, internal); put16(b, suggested);
    put_mapped_v4(b, 0);
    return b;
}
inline bool pcp_parse_map(const uint8_t *d, size_t n, const uint8_t nonce[12], uint16_t internal, uint16_t &ext_port, uint32_t &ext_ip, uint32_t &lifetime) {
    if (n < 60 || d[0] != 2 || d[1] != 0x81 || d[3] != 0 || std::memcmp(d + 24, nonce, 12) != 0 || get16(d + 40) != internal) return false;
    lifetime = get32(d + 4);
    ext_port = (uint16_t)get16(d + 42);
    ext_ip = get32(d + 56);   // low 4 bytes of the 16-byte address
    return true;
}

// ---------------------------------------------------------------------------- UPnP IGD
inline std::string ssdp_search(const char *st) {
    return std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
}
inline std::string lower(std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }
inline std::string header_value(const std::string &msg, const char *name) {
    const std::string l = lower(msg), key = lower(std::string("\n") + name + ":");
    size_t p = l.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    while (p < msg.size() && (msg[p] == ' ' || msg[p] == '\t')) ++p;
    size_t e = msg.find_first_of("\r\n", p);
    return msg.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
struct Url { std::string host, path; uint16_t port = 80; };
inline bool parse_http_url(const std::string &u, Url &out) {
    if (u.compare(0, 7, "http://") != 0) return false;
    size_t s = 7, slash = u.find('/', s);
    std::string hp = u.substr(s, slash == std::string::npos ? std::string::npos : slash - s);
    out.path = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hp.rfind(':');
    out.port = 80;
    if (colon != std::string::npos) { out.port = (uint16_t)std::atoi(hp.c_str() + colon + 1); hp.resize(colon); }
    out.host = hp;
    return !out.host.empty();
}
inline std::string xml_tag(const std::string &x, const char *tag, size_t from = 0, size_t *end = nullptr) {
    const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t a = x.find(open, from);
    if (a == std::string::npos) return {};
    a += open.size();
    size_t b = x.find(close, a);
    if (b == std::string::npos) return {};
    if (end) *end = b + close.size();
    return x.substr(a, b - a);
}
// Finds a WANIPConnection / WANPPPConnection service in a device description.
inline bool upnp_find_service(const std::string &xml, std::string &service_type, std::string &control_url) {
    size_t pos = 0;
    for (;;) {
        size_t end = 0;
        const size_t svc = xml.find("<service>", pos);
        if (svc == std::string::npos) return false;
        const std::string block = xml_tag(xml, "service", svc, &end);
        pos = end ? end : svc + 9;
        const std::string type = xml_tag(block, "serviceType");
        if (type.find(":WANIPConnection:") != std::string::npos || type.find(":WANPPPConnection:") != std::string::npos) {
            service_type = type; control_url = xml_tag(block, "controlURL");
            if (!control_url.empty()) return true;
        }
    }
}
inline std::string soap_envelope(const std::string &service, const std::string &action, const std::string &args) {
    return "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
           "<s:Body><u:" + action + " xmlns:u=\"" + service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
}
inline std::string upnp_add_mapping_args(uint16_t ext, uint16_t internal, const std::string &client, uint32_t lease) {
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(ext) + "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" +
           std::to_string(internal) + "</NewInternalPort><NewInternalClient>" + client + "</NewInternalClient><NewEnabled>1</NewEnabled>"
           "<NewPortMappingDescription>Eve of Destruction</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
}
inline bool parse_ipv4(const std::string &s, uint32_t &ip) {
    unsigned a, b, c, d; char tail;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip = a << 24 | b << 16 | c << 8 | d; return true;
}

}  // namespace netplay_online
