# Modern hardware: PS2 limits lifted

Rule (AGENTS.md): a PS2-era limit is overcome, adaptively, inside the task that hits it. This page records each
limit that has been lifted, how, and what it costs.

## Guest main memory: 32 MB -> 128 MB (13 Nov 2026)

**Player problem.** Leaving a Career back to the main menu after a longer session showed the "SOMETHING WENT
WRONG" screen. crash.log: `MemAlloc(song.sfx, 1760704): exceeded stack` — the game's main memory pool was full.

**Cause.** At boot the game sizes its main pool (`STAK` pool at `0x3CC438`, `MemAlloc 0x12DDB8`) by probing its
own malloc in 256 KB steps from 25 MB up (`0x12DA60`-`0x12DADC`) and takes everything it can reach. Its heap ends
at the main-thread stack, which crt0 places just under the console's 32 MB, so the pool was ~26.5 MB
(`stack range: 0x4e9380 --> 0x1f69380` in the game's own log).

**Change.**
- `EE_RAM_SIZE` (`runtime/include/ee.h`) is 128 MB. The guest RAM is one `calloc`; macOS maps large zeroed blocks
  lazily, so a page costs host memory only once the game writes to it. Nothing is reserved or filled up front.
- `SetupThread` (`runtime/kernel/kernel.cpp`) always puts the main stack at the top of guest RAM, so the heap (and
  the game's pool) reaches it: the pool is now ~122.5 MB (`stack range: 0x4e9380 --> 0x7f69380`).
- DMA addresses wrap at `EE_RAM_SIZE` (`runtime/hw/hw.cpp dma_ptr`), the CD stream status word and the developer
  watch tool no longer mask to 32 MB.

**Evidence.** Scripted Career start -> Pause -> Quit -> Exit Career Mode reaches the front end (Race / Dare / Save)
with no panic; boot log shows the 122.5 MB pool. Unit suite 90/90.

**Limits that remain.** Guest addresses below `0x10000000` are RAM; 128 MB leaves room to raise again (power of
two, below I/O space) if content ever needs it.

## Developer: faster-than-real-time test runs

`EOD_TIME_SCALE=N` (1-64, headless test runs only, `runtime/platform/posix/platform_posix.cpp`) runs the game clock
N times faster than real time. The game still sees 60 Hz frames (vblanks, alarms, pad scripts are all on that
clock), so a scripted route that needs a minute of menus finishes in seconds. `EOD_TIME_SCALE=8` measured ~6.5x
(11,700 frames in 30 s). Do not use it for timing measurements or online runs.
