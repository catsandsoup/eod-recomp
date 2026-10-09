// hle_fs.cpp - file system selection.
//
// The engine has two file system back ends: a stream.irx based one that
// reads raw CD sectors located through a file table, and the developer
// "host0:" back end built on the standard fileio RPC.  The port always uses
// the latter; the HLE fileio server maps host0: onto the game directory.
#include "core/cpu.h"
#include "funcs.h"

extern "C" void f_0033DD90(void) { ret_s32(0); }
