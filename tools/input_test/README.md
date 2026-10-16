# Controller protocol checks

`eod_input_test` uses SDL virtual joysticks; `eod_pad_rpc_test` exercises
padman RPC and its EE/IOP buffers with host input/vibration stubs.

The current open-source [PS2SDK libpad implementation](https://github.com/ps2dev/ps2sdk/blob/8451bd58c9c76d6cbb0160fff52d9635f76c8069/ee/rpc/pad/src/libpad.c)
sends `padSetActDirect` as RPC command 7. Its actuator/alignment result is
word 5 (+0x14), unlike OPEN/CLOSE's result in word 3 (+0x0c).

That RPC producer alone does **not** cover this game's rumble. Read-only
inspection of the owner's NTSC-U executable (SDK version string 2800)
shows a different producer:

- Game call at `0x00333608` invokes the actuator setter `0x00390678`.
- Setter checks pad `currentTask == 1`, copies six actuator bytes to a
  separate outgoing packet at +12, writes command 1 at +4 and length 6 at +8,
  and invokes `0x0038F878`.
- Sender increments the packet's 32-bit sequence at +0 and DMA-transfers
  32 bytes to the IOP buffer returned by OPEN, plus `(sequence & 1) * 32`.
- The sequence is independent of the IOP-to-EE pad status frame counter.
- Alignment setter `0x00390738` uses RPC command 8 and reads its reply result
  at +0x14 (`0x003907C8`).

The DMA regression test reconstructs those producer packets (no executable
bytes or generated recompilation code are included). It covers nonzero and
zero motors, alternating halves, multiple packets between vblanks, stale
packets, invalid command/length and close/reopen with a fresh sequence.
Before the DMA receiver and alignment reply fix, seven assertions failed.
Both RPC and DMA producers now route actuator slots through `SET_ACTALIGN`.
Physical USB access, the hardware mapping and actual motors still require
the physical-pad diagnostic described in `docs/MACOS.md`.
