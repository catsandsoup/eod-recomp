// iop.h - HLE IOP: RPC server registry used by the module emulations.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "../core/memory.h"

// An RPC call as seen by an HLE server.
struct RpcCall {
    uint32_t sid;
    uint32_t fno;        // rpc_number
    uint8_t *in;         // send data (in IOP RAM)
    uint32_t in_size;
    uint32_t ee_recv;    // EE address for the reply (may be 0)
    uint32_t recv_size;  // reply size requested by the client
    uint32_t mode;       // rmode
};

// Handler: returns the number of bytes of `out` to copy to the EE reply
// buffer (truncated to recv_size).  `out` is a scratch buffer of 64KB that
// initially contains the send data (servers commonly reply in place).
using RpcHandler = std::function<uint32_t(const RpcCall &call, uint8_t *out)>;

void iop_register_server(uint32_t sid, const char *name, RpcHandler h);
// Sends a SIF command packet to the EE (delivered after the current RPC
// reply when called from an RPC handler).
void iop_send_cmd(uint32_t cid, uint32_t opt, const void *payload, uint32_t size);
bool iop_has_server(uint32_t sid);

// Module loading (called by the loadfile server)
int iop_load_module(const std::string &path, const std::string &args);

// IOP heap for HLE buffers
uint32_t iop_alloc(uint32_t size);

// Modules
void iop_modules_init();
void iop_fileio_init();
void iop_cdvd_init();
void iop_pad_init();
void iop_mc_init();
void iop_sound_init();
void iop_misc_init();

// Root directory of the game data (contains DATA/...)
extern std::string g_game_root;
