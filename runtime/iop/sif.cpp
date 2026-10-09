// sif.cpp - IOP side of the SIF command / RPC protocol.
//
// The EE libraries (libkernel's sifcmd/sifrpc, libcdvd, libpad, ...) run
// unmodified.  They send command packets to the IOP with SifSetDma; here we
// decode those packets, dispatch RPC calls to HLE servers and answer with
// RPC_END packets written into the EE receive buffer followed by a SIF0
// (DMA channel 5) interrupt, which is what the real IOP does.
#include "sif.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "../core/log.h"
#include "../hw/hw.h"
#include "../kernel/kernel.h"
#include "iop.h"

namespace {

constexpr uint32_t kIopCmdBuf = 0x00001000;  // IOP address of the command buffer
constexpr uint32_t kIopHeapBase = 0x00010000;

enum : uint32_t {
    SIF_CMD_CHANGE_SADDR = 0x80000000,
    SIF_CMD_SET_SREG = 0x80000001,
    SIF_CMD_INIT_CMD = 0x80000002,
    SIF_CMD_RESET_CMD = 0x80000003,
    SIF_CMD_RPC_END = 0x80000008,
    SIF_CMD_RPC_BIND = 0x80000009,
    SIF_CMD_RPC_CALL = 0x8000000A,
    SIF_CMD_RPC_RDATA = 0x8000000C,
};

struct Server {
    std::string name;
    RpcHandler handler;
    uint32_t buff;   // IOP receive buffer
    uint32_t sdata;  // fake server struct address
};

std::map<uint32_t, Server> g_servers;
uint32_t g_ee_pktbuf;      // EE command receive buffer
uint32_t g_sregs[32];
uint32_t g_mainaddr;
uint32_t g_msflag, g_smflag = 0x00070000;  // SIFINIT | CMDINIT | BOOTEND
uint32_t g_iop_heap = kIopHeapBase;
uint32_t g_dma_id = 1;
std::deque<std::vector<uint8_t>> g_out;  // packets waiting to go to the EE
bool g_out_busy;
std::vector<uint8_t> g_scratch(0x10000);
std::vector<std::vector<uint8_t>> g_post;  // queued by iop_send_cmd during a call
bool g_in_call;

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

void deliver_next();

void send_to_ee(std::vector<uint8_t> pkt) {
    g_out.push_back(std::move(pkt));
    if (!g_out_busy) deliver_next();
}

// Writes one packet into the EE buffer and raises the SIF0 interrupt.
void deliver_next() {
    if (g_out.empty()) {
        g_out_busy = false;
        return;
    }
    if (!g_ee_pktbuf) {
        WARN("SIF: EE receive buffer unknown, dropping packet");
        g_out.clear();
        return;
    }
    std::vector<uint8_t> pkt = std::move(g_out.front());
    g_out.pop_front();
    ee_write_block(g_ee_pktbuf, pkt.data(), (uint32_t)pkt.size());
    g_out_busy = true;
    LOG(LOG_SIF, "-> EE cmd %08x (%zu bytes) at %08x", rd32(&pkt[8]), pkt.size(), g_ee_pktbuf);
    dmac_channel_done(DMA_SIF0);
}

std::vector<uint8_t> make_packet(uint32_t cid, uint32_t size) {
    std::vector<uint8_t> p(size, 0);
    wr32(&p[0], size & 0xFF);  // psize, dsize = 0
    wr32(&p[4], 0);
    wr32(&p[8], cid);
    wr32(&p[12], 0);
    return p;
}

void rpc_end(uint32_t rec_id, uint32_t pkt_addr, uint32_t rpc_id, uint32_t client, uint32_t cid,
             uint32_t server, uint32_t buff, uint32_t cbuff) {
    auto p = make_packet(SIF_CMD_RPC_END, 0x30);
    wr32(&p[16], rec_id);
    wr32(&p[20], pkt_addr);
    wr32(&p[24], rpc_id);
    wr32(&p[28], client);
    wr32(&p[32], cid);
    wr32(&p[36], server);
    wr32(&p[40], buff);
    wr32(&p[44], cbuff);
    send_to_ee(std::move(p));
}

void handle_bind(const uint8_t *pkt) {
    uint32_t rec_id = rd32(pkt + 16), pkt_addr = rd32(pkt + 20), rpc_id = rd32(pkt + 24);
    uint32_t client = rd32(pkt + 28), sid = rd32(pkt + 32);
    auto it = g_servers.find(sid);
    if (it == g_servers.end()) {
        // Unknown service: answer with a stub that logs what is asked of it
        // so missing functionality is easy to spot.
        WARN("SIF: bind to unknown RPC server %08x - using a logging stub", sid);
        char name[32];
        snprintf(name, sizeof name, "unknown_%08x", sid);
        iop_register_server(sid, name, [sid](const RpcCall &c, uint8_t *buf) -> uint32_t {
            static std::map<uint64_t, int> seen;
            if (seen[((uint64_t)sid << 32) | c.fno]++ < 3) {
                char hex[3 * 32 + 1] = {};
                for (uint32_t i = 0; i < 32 && i < c.in_size; i++) snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
                WARN("SIF stub %08x fno=%u in=%u recv=%u: %s", sid, c.fno, c.in_size, c.recv_size, hex);
            }
            memset(buf, 0, c.recv_size < 0x10000 ? c.recv_size : 0x10000);
            return c.recv_size;
        });
        it = g_servers.find(sid);
    }
    LOG(LOG_SIF, "bind %08x (%s) client=%08x", sid, it->second.name.c_str(), client);
    rpc_end(rec_id, pkt_addr, rpc_id, client, SIF_CMD_RPC_BIND, it->second.sdata, it->second.buff,
            it->second.buff);
}

uint32_t g_server_by_sdata(uint32_t sdata, Server **out) {
    for (auto &kv : g_servers)
        if (kv.second.sdata == sdata) {
            *out = &kv.second;
            return kv.first;
        }
    *out = nullptr;
    return 0;
}

void handle_call(const uint8_t *pkt) {
    uint32_t rec_id = rd32(pkt + 16), pkt_addr = rd32(pkt + 20), rpc_id = rd32(pkt + 24);
    uint32_t client = rd32(pkt + 28), fno = rd32(pkt + 32), send_size = rd32(pkt + 36);
    uint32_t receive = rd32(pkt + 40), recv_size = rd32(pkt + 44), rmode = rd32(pkt + 48);
    uint32_t server = rd32(pkt + 52);
    Server *s;
    uint32_t sid = g_server_by_sdata(server, &s);
    if (!s) {
        WARN("SIF: call to unknown server data %08x (fno %d)", server, fno);
        rpc_end(rec_id, pkt_addr, rpc_id, client, SIF_CMD_RPC_CALL, server, 0, 0);
        return;
    }
    RpcCall call{sid, fno, g_iop_ram + s->buff, send_size, receive, recv_size, rmode};
    if (send_size > g_scratch.size()) send_size = (uint32_t)g_scratch.size();
    memcpy(g_scratch.data(), g_iop_ram + s->buff, send_size);
    LOG(LOG_SIF, "call %s fno=%d send=%u recv=%08x/%u", s->name.c_str(), fno, send_size, receive, recv_size);
    g_in_call = true;
    uint32_t n = s->handler(call, g_scratch.data());
    g_in_call = false;
    if (receive && n) {
        if (n > recv_size) n = recv_size;
        ee_write_block(receive, g_scratch.data(), n);
    }
    rpc_end(rec_id, pkt_addr, rpc_id, client, SIF_CMD_RPC_CALL, server, 0, 0);
    // commands the server wants delivered after its reply (async completions)
    for (auto &p : g_post) send_to_ee(std::move(p));
    g_post.clear();
}

void handle_rdata(const uint8_t *pkt) {
    // SifRpcOtherDataPkt: rec_id, pkt_addr, rpc_id, client, src(IOP), dest(EE), size
    uint32_t rec_id = rd32(pkt + 16), pkt_addr = rd32(pkt + 20), rpc_id = rd32(pkt + 24);
    uint32_t client = rd32(pkt + 28), src = rd32(pkt + 32), dest = rd32(pkt + 36), size = rd32(pkt + 40);
    if (src + size <= IOP_RAM_SIZE) ee_write_block(dest, g_iop_ram + src, size);
    rpc_end(rec_id, pkt_addr, rpc_id, client, SIF_CMD_RPC_RDATA, 0, 0, 0);
}

void handle_command(const uint8_t *pkt, uint32_t size) {
    uint32_t cid = rd32(pkt + 8);
    LOG(LOG_SIF, "<- EE cmd %08x size %u opt %08x", cid, size, rd32(pkt + 12));
    switch (cid) {
    case SIF_CMD_CHANGE_SADDR:
        g_ee_pktbuf = rd32(pkt + 16);
        break;
    case SIF_CMD_SET_SREG:
        if (rd32(pkt + 16) < 32) g_sregs[rd32(pkt + 16)] = rd32(pkt + 20);
        break;
    case SIF_CMD_INIT_CMD:
        if (rd32(pkt + 12) == 0) {
            g_ee_pktbuf = rd32(pkt + 16);
            LOG(LOG_SIF, "EE command buffer at %08x", g_ee_pktbuf);
        }
        break;
    case SIF_CMD_RESET_CMD: {
        char arg[81] = {};
        memcpy(arg, pkt + 24, 80);
        LOG(LOG_SIF, "IOP reset requested: '%s'", arg);
        break;
    }
    case SIF_CMD_RPC_BIND:
        handle_bind(pkt);
        break;
    case SIF_CMD_RPC_CALL:
        handle_call(pkt);
        break;
    case SIF_CMD_RPC_RDATA:
        handle_rdata(pkt);
        break;
    default:
        WARN("SIF: unhandled command %08x", cid);
    }
}

}  // namespace

// ------------------------------------------------------------- public API
void iop_send_cmd(uint32_t cid, uint32_t opt, const void *payload, uint32_t size) {
    std::vector<uint8_t> p = make_packet(cid, 16 + size);
    wr32(&p[12], opt);
    if (size) memcpy(&p[16], payload, size);
    if (g_in_call)
        g_post.push_back(std::move(p));
    else
        send_to_ee(std::move(p));
}

void iop_register_server(uint32_t sid, const char *name, RpcHandler h) {
    Server s;
    s.name = name;
    s.handler = std::move(h);
    s.buff = iop_alloc(0x10000);
    s.sdata = iop_alloc(0x40);
    g_servers[sid] = std::move(s);
}

bool iop_has_server(uint32_t sid) { return g_servers.count(sid) != 0; }

uint32_t iop_alloc(uint32_t size) {
    uint32_t a = g_iop_heap;
    g_iop_heap = (g_iop_heap + size + 63) & ~63u;
    if (g_iop_heap > IOP_RAM_SIZE) fatal("IOP HLE heap exhausted");
    return a;
}

void sif_init() {
    iop_modules_init();
}

uint32_t sif_set_dma(uint32_t list, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t t = list + i * 16;
        uint32_t src = ee_r32(t), dest = ee_r32(t + 4), size = ee_r32(t + 8), attr = ee_r32(t + 12);
        LOG(LOG_SIF, "SifSetDma src=%08x dest=%08x size=%x attr=%x", src, dest, size, attr);
        if (dest == kIopCmdBuf) {
            std::vector<uint8_t> pkt(size);
            ee_read_block(src, pkt.data(), size);
            if (size >= 16) handle_command(pkt.data(), size);
        } else if (dest + size <= IOP_RAM_SIZE) {
            ee_read_block(src, g_iop_ram + dest, size);
        } else {
            WARN("SIF: DMA to bad IOP address %08x size %x", dest, size);
        }
    }
    return g_dma_id++;
}

int sif_dma_stat(int) { return -1; }  // completed

void sif_set_dchain() {}

void sif_set_reg(uint32_t reg, uint32_t v) {
    LOG(LOG_SIF, "SifSetReg(%08x, %08x)", reg, v);
    if (reg == 1) g_mainaddr = v;
    else if (reg == 3) g_msflag |= v;
    else if (reg == 4) { /* the HLE IOP is always booted: flags stay set */ }
    else if (reg & 0x80000000u) g_sregs[(reg & 0x1F)] = v;
}

uint32_t sif_get_reg(uint32_t reg) {
    uint32_t v;
    switch (reg) {
    case 1: v = g_mainaddr; break;
    case 2: v = kIopCmdBuf; break;
    case 3: v = g_msflag; break;
    case 4: v = g_smflag; break;
    case 0x80000000: v = kIopCmdBuf; break;  // SIF_SYSREG_SUBADDR
    case 0x80000001: v = g_ee_pktbuf; break;
    case 0x80000002: v = 1; break;          // RPC initialised
    default: v = (reg & 0x80000000u) ? g_sregs[reg & 0x1F] : 0;
    }
    LOG(LOG_SIF, "SifGetReg(%08x) = %08x", reg, v);
    return v;
}

uint32_t sif_read_reg(uint32_t off) {
    switch (off) {
    case 0xF200: return g_mainaddr;
    case 0xF210: return kIopCmdBuf;
    case 0xF220: return g_msflag;
    case 0xF230: return g_smflag;
    case 0xF240: return 0xF0000102;
    }
    return 0;
}

void sif_write_reg(uint32_t off, uint32_t v) {
    switch (off) {
    case 0xF200: g_mainaddr = v; break;
    case 0xF220: g_msflag |= v; break;
    case 0xF230: break;
    }
}

// The EE's SIF0 interrupt handler has consumed the previous packet.
void sif_after_sif0_irq() {
    g_out_busy = false;
    deliver_next();
}

void sif_update() {}
