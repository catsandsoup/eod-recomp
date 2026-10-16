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

## Save protocol audit

`eod_mc_test` sends the actual SDK 2.8 request layouts to mcserv, writes two
independent cards, terminates the writer, and starts a separate reader.
It verifies the exact native card paths as well as contents, so a shared
client/server offset mistake cannot pass merely by reading back its own
wrong filename. It also checks small inline writes, unaligned write heads,
short EOF reads, callback fix-up sizes, seek-to-end length and Delete.

Target Open `0x0038C080` copies its filename to `0x004DE004` and sends a
0x414-byte packet starting at `0x004DFFF0`: the filename starts at +0x14,
not +0x18. The old handler lost the first four characters on Open/Mkdir and
Delete. Four exact-path assertions failed before correction. Existing saves
written by that handler may live under shortened directory names; this fix
does not migrate or delete them.

The save writer `0x003380A0` opens with flag 0x200, invokes Write
`0x0038C530`, waits for completion, then closes. Write sends up to 16 inline
head bytes and the remainder from an aligned EE buffer. The public
[PS2SDK libmc](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/memorycard/src/libmc.c)
uses the same name/descriptor field layouts. Although a public header calls
0x200 equivalent to O_CREAT, the
[MCMAN consumer](https://github.com/ps2dev/ps2sdk/blob/master/iop/memorycard/mcman/src/ps2mc_fio.c)
explicitly recreates existing files for that bit; host truncation is correct.

Additional target reply reads: SET_MMODE `0x0039062C` reads +0x14,
SET_BTNINFO `0x00390930` reads +0x10, and SET_VREF `0x00390B0C` reads +0x1C.
Tests initialize those fields independently of arguments; all three failed
with the old universal +0x0C reply, and pass with command-specific offsets.

The persistence test proves the host-backed RPC path across processes. It
is not proof of an actual career save/load through the game's menus.
Known remaining MC approximations include fixed free space, directory
listing/metadata commands, formatting and current-directory handling.

## Explicit controller layouts

The application setting `[Input] ControllerLayout=original|modern` defaults
to `original`. Original retains the PS2 mapping; Modern maps RT to Cross
pressure (gas) and LT to Square pressure (brake), releasing their native
L2/R2 roles. Source inspection of `0x00332D64` and `0x002AA490` established
that the game's classic analog throttle/brake read Cross/Square pressure,
while its third layout uses digital shoulders. Exposing L2/R2 pressure
alone therefore does not meet the modern analog driving requirement.

Virtual joystick tests cover partial and light trigger travel, releases,
independent players, return to Original, and face-button/shared-keyboard
priority. A held face button/key retains full pressure even when a trigger
is partly pulled. The platform's final digital-pressure fill only supplies
255 when a pressed button has zero pressure, preserving partial travel.
Input logs include Cross/Square pressure changes as well as L2/R2.

These hardware-free mapping tests do not verify a physical Rock Candy pad
or claim a complete actual-game analog-trigger driving test.
