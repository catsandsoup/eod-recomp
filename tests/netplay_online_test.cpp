#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include "netplay_online_wire.h"
using namespace netplay_online;
int main() {
    // SHA-1 known answers (also across the 64-byte block boundary)
    assert(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    assert(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    assert(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    assert(hex(sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // codes: any case, spaces and dashes; no 0/O/1/I/L ever
    std::string c;
    assert(normalize_code("k7qd2", c) == CodeCheck::Ok && c == "K7QD2");
    assert(normalize_code(" K7Q-D2\n", c) == CodeCheck::Ok && c == "K7QD2");
    assert(normalize_code("K7QD", c) == CodeCheck::TooShort);
    assert(normalize_code("K7QD23", c) == CodeCheck::TooLong);
    assert(normalize_code("K7QO2", c) == CodeCheck::BadChar);
    assert(normalize_code("K7Q12", c) == CodeCheck::BadChar);
    assert(std::strlen(code_alphabet()) == 31);
    uint8_t rnd[5] = {0, 31, 62, 200, 255};
    c = code_from_bytes(rnd, 5);
    { std::string c2; assert(c.size() == 5 && normalize_code(c, c2) == CodeCheck::Ok && c2 == c); }
    // rendezvous keys differ by role and by secret, and are stable
    assert(rendezvous_hash('H', "K7QD2") != rendezvous_hash('J', "K7QD2"));
    assert(rendezvous_hash('H', "K7QD2") != rendezvous_hash('H', "K7QD3"));
    assert(rendezvous_hash('H', "K7QD2") == rendezvous_hash('H', "K7QD2"));
    assert(join_tag("K7QD2") != join_tag("K7QD3"));
    // private / shared address ranges
    assert(is_private_or_shared_ipv4(0x0A000001) && is_private_or_shared_ipv4(0xC0A80101) && is_private_or_shared_ipv4(0x64400001) && is_private_or_shared_ipv4(0xAC100001));
    assert(!is_private_or_shared_ipv4(0x08080808) && !is_private_or_shared_ipv4(0xCA807508));
    // STUN: request shape, XOR-MAPPED-ADDRESS response
    uint8_t tid[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    auto req = stun_request(tid);
    assert(req.size() == 20 && looks_like_stun(req.data(), req.size()) && req[0] == 0 && req[1] == 1);
    {
        std::vector<uint8_t> r;
        put16(r, 0x0101); put16(r, 12); put32(r, kStunCookie); r.insert(r.end(), tid, tid + 12);
        put16(r, 0x0020); put16(r, 8); r.push_back(0); r.push_back(1);
        put16(r, 40000 ^ (kStunCookie >> 16)); put32(r, 0xCA807508u ^ kStunCookie);
        uint32_t ip; uint16_t port;
        assert(stun_parse_response(r.data(), r.size(), tid, ip, port) && ip == 0xCA807508u && port == 40000);
        uint8_t other[12] = {9};
        assert(!stun_parse_response(r.data(), r.size(), other, ip, port));
        assert(!looks_like_stun((const uint8_t *)"\x7D\x3E\x01\x00", 4));   // the game's own packets are never STUN
    }
    // tracker: connect and announce codecs
    {
        auto cr = tracker_connect(77);
        assert(cr.size() == 16 && get64(cr.data()) == kTrackerProtocolId);
        std::vector<uint8_t> resp; put32(resp, 0); put32(resp, 77); put64(resp, 0x1122334455667788ull);
        uint32_t tid2; uint64_t cid;
        assert(tracker_parse_connect(resp.data(), resp.size(), tid2, cid) && tid2 == 77 && cid == 0x1122334455667788ull);
        uint8_t pid[20] = {};
        auto an = tracker_announce(cid, 78, rendezvous_hash('H', "K7QD2"), pid, 0, TE_STARTED, 27015, 50);
        assert(an.size() == 98 && get32(an.data() + 8) == 1 && get32(an.data() + 12) == 78 && get16(an.data() + 96) == 27015 && get32(an.data() + 80) == 2);
        std::vector<uint8_t> ar; put32(ar, 1); put32(ar, 78); put32(ar, 1800); put32(ar, 0); put32(ar, 1);
        put32(ar, 0x01020304); put16(ar, 5555); put32(ar, 0x0A0B0C0D); put16(ar, 6666);
        uint32_t t3, interval; std::vector<TrackerPeer> peers;
        assert(tracker_parse_announce(ar.data(), ar.size(), t3, interval, peers) && t3 == 78 && interval == 1800 && peers.size() == 2);
        assert(peers[1].ip == 0x0A0B0C0D && peers[1].port == 6666);
        std::vector<uint8_t> er; put32(er, 3); put32(er, 78); uint32_t t4;
        assert(tracker_is_error(er.data(), er.size(), t4) && t4 == 78);
    }
    // NAT-PMP and PCP
    {
        auto m = pmp_map_request(27015, 27015, 1800);
        assert(m.size() == 12 && m[1] == 1 && get16(m.data() + 4) == 27015);
        std::vector<uint8_t> r; r.push_back(0); r.push_back(129); put16(r, 0); put32(r, 99); put16(r, 27015); put16(r, 40123); put32(r, 1800);
        uint16_t ep; uint32_t life;
        assert(pmp_parse_map(r.data(), r.size(), 27015, ep, life) && ep == 40123 && life == 1800);
        std::vector<uint8_t> e; e.push_back(0); e.push_back(128); put16(e, 0); put32(e, 5); put32(e, 0xCA807508u);
        uint32_t eip; assert(pmp_parse_external(e.data(), e.size(), eip) && eip == 0xCA807508u);
        uint8_t nonce[12] = {7, 7, 7};
        auto p = pcp_map_request(0xC0A80102, nonce, 27015, 27015, 1800);
        assert(p.size() == 60 && p[0] == 2 && p[1] == 1 && p[36] == 17 && get16(p.data() + 40) == 27015);
        std::vector<uint8_t> pr(60, 0); pr[0] = 2; pr[1] = 0x81; 
        pr[4] = 0; pr[5] = 0; pr[6] = 7; pr[7] = 8;   // lifetime 1800
        std::memcpy(pr.data() + 24, nonce, 12);
        pr[40] = 27015 >> 8; pr[41] = 27015 & 255; pr[42] = 0x9C; pr[43] = 0x43;
        pr[54] = 0xFF; pr[55] = 0xFF; pr[56] = 0xCA; pr[57] = 0x80; pr[58] = 0x75; pr[59] = 0x08;
        uint16_t pe; uint32_t pip, plife;
        assert(pcp_parse_map(pr.data(), pr.size(), nonce, 27015, pe, pip, plife) && pe == 0x9C43 && pip == 0xCA807508u && plife == 1800);
    }
    // UPnP: SSDP reply, device description, URLs
    {
        std::string reply = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nLocation: http://192.168.1.1:5000/rootDesc.xml\r\nST: upnp:rootdevice\r\n\r\n";
        Url u; assert(parse_http_url(header_value(reply, "location"), u) && u.host == "192.168.1.1" && u.port == 5000 && u.path == "/rootDesc.xml");
        std::string xml = "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/l3f</controlURL></service>"
                          "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/IPConn</controlURL></service></serviceList></device></root>";
        std::string type, ctl;
        assert(upnp_find_service(xml, type, ctl) && ctl == "/ctl/IPConn" && type.find("WANIPConnection") != std::string::npos);
        assert(!upnp_find_service("<root></root>", type, ctl));
        std::string args = upnp_add_mapping_args(27015, 27015, "192.168.1.20", 1800);
        assert(args.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos && args.find("192.168.1.20") != std::string::npos);
        assert(soap_envelope(type, "AddPortMapping", args).find("u:AddPortMapping") != std::string::npos);
        assert(xml_tag("<a><NewExternalIPAddress>8.8.4.4</NewExternalIPAddress></a>", "NewExternalIPAddress") == "8.8.4.4");
        uint32_t ip; assert(parse_ipv4("8.8.4.4", ip) && ip == 0x08080404 && !parse_ipv4("8.8.4", ip) && !parse_ipv4("300.1.1.1", ip));
    }
    std::puts("netplay_online: ok");
}
