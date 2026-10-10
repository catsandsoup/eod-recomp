// offload.h - the VIF1/VU1, GIF DMA and GS command stream on a worker thread.
//
// On the PS2 these run alongside the EE: the EE kicks a DMA chain and goes
// on with the game logic while VIF1 feeds VU1 and the GIF.  With the worker
// (two or more spare hardware threads), DMA channels 1 and 2, the VIF1/GIF
// FIFOs and registers, VU1 started from the EE, and every command the EE
// sends to the GS (privileged registers, vblank) are queued and executed in
// order on one thread, the only producer of the GS command stream.  The EE
// waits for the worker only where it needs a result: reads of VU1 memory,
// VIF1 registers and GS state, and polls of a busy channel.
#pragma once
#include <functional>

void offload_start();
bool offload_active();
// true on the worker thread itself
bool offload_in_worker();
// Runs fn on the worker after everything queued before it (or right away
// when the worker is off or this is the worker).
void offload_run(std::function<void()> fn);
// Waits until the worker has run everything queued.
void offload_sync();
// Waits until pred() holds or the worker has nothing left to do (pred is
// re-checked whenever the worker finishes a job).
void offload_wait(const std::function<bool()> &pred);
// The worker finished something a waiter may be interested in.
void offload_notify();
// Whether the worker has queued or running work (VPU_STAT).
bool offload_busy();
