# Worker lifetime at process exit

The `--frames` limit, window close, guest Exit/ExecOSD, normal kernel return
and standalone replay return call `gs_shutdown()` before normal C++ static
destruction. Shutdown runs on the EE/main thread and is idempotent:

1. Stop and join the ticker so it cannot raise new kernel events.
2. Drain and join the optional VIF/GIF offload producer.
3. Flush the final GS command chunk and wait for all submitted chunks.
4. Stop and join the GS worker. Its final raster barrier and hardware finish
   run on the thread that owns rendering.
5. Drain and join software raster workers, then release their job ring.

The GS, offload, raster and ticker workers now retain joinable handles. Quit
continues to exit normally with the requested status; static destruction is
not bypassed. Abnormal aborts are unchanged.

This addresses the observed exit race where the main thread destroyed the
Vulkan texture unordered_map from `exit`/`gs_vblank` while the GS worker was
still in `vram_texture`/`draw_triangle`/`run_cmds`.

`WorkerShutdown` is a CPU-only subprocess regression using the production
VIF/GIF offload and POSIX ticker implementations. It verifies active and queued
cache-writing jobs finish before owner destruction, repeated stop/start,
idempotence, callback cessation, and normal child-process exit. Production GS,
raster, kernel and main objects are compiled separately. The unit test does
not exercise a GPU or establish that the original 6600-vblank game run now
exits cleanly; that remains an integration check. Windows ticker stop/join is
implemented with a retained HANDLE, but was not compiled on macOS.
