# Player journey — PS2 Mac port

The owner's GOALS.md, PRODUCT_RULES.md and MODDING_IDEAS.md from `TDEOD RECOMP`
define the player outcome. Their Xbox/NV2A implementation details are historical;
the active target is PS2 SLUS_209.10 on Apple Silicon through Vulkan/MoltenVK.
Faithful physics, damage, AI and progression remain required within these flows.
120 Hz and all mod ideas stay deferred until the ordinary game works well.

The owner's latest priority is custom single-player matches and local multiplayer
at 100% game speed, with 4K-or-higher internal rendering and correctly proportioned
16:10/16:9 presentation. Career testing is now owned by the user. Keyboard controls
must follow racing conventions: WASD/arrows to drive, Space to brake, Enter to
accept menus, and Esc to go back/pause. Screenshot milestones and verified source
checkpoints should be shared periodically.

| Journey | Acceptance | Evidence and remaining work |
| --- | --- | --- |
| Install and first launch | Open a self-contained app, select the owner's disc once, see progress/friendly errors, reach driving within a minute | Real ISO import/CRC, staged replacement, malformed/cancelled imports and relocated signed GPU app tested. Native dialogs and complete first-launch time still need direct validation. |
| Return to the game | No repeat disc picker or terminal/dependency setup | Application Support disc/config/memcard paths implemented. Relocated app has its loader/driver/libusb. Returning UI launch remains a hands-on check. |
| Start a race | Working title/menu navigation, original event/track rules and smooth ordinary gameplay | Single-title/menu navigation and two-player Jump Race captured. Broader event modes, damage, slow-mo and AI behavior need coverage. |
| Play together | Keyboard+pad or two pads, independent players; Rock Candy first; usable analog throttle/brake and rumble | Virtual pad and SDK packet tests plus distinct two-player steering captured. Target SDK DMA rumble fixed. Physical device/motors absent; Modern RT/LT mapping is implemented and covered by virtual tests; actual partial-pressure driving is next. |
| Change settings | Reachable UI, saved settings and effects matching controls | F1/Cmd+, settings with native checkboxes/popups, persisted config and auto resolution. Correct 16:10 camera/composition is being integrated. Volume pipeline regression found and fixed; native UI remains compile-tested only. |
| Save, quit and return | Career progress and options survive process restart; no damage to other saves | Target memcard packet filename-offset bug found; protocol and separate-process tests added. Actual game saved a changed nickname and restored it in a new process. Further career testing is left to the owner. |
| Watch and listen | Original movies/music/effects behave correctly without stutter | Muted/offline audio tests only. Profile identifies translated bitbuffer helpers as intro bottleneck. Mixer effect accuracy and original movie behavior still need work. |

Tests must run headless and muted under the owner's standing preference.
Screenshots, offline audio dumps, protocol tests and process-restart tests are
useful evidence; unit tests alone do not establish the complete player journey.
No copyrighted disc/generated game data belongs in source or public downloads.
All current builds are personal local builds.

## Local multiplayer: translating the original prompt

The Mac journey must explain the assigned device before showing “Press X for
player 2.” Here X is PS2 Cross/Accept, not the Mac X key. The Racing keyboard's
menu Accept is Enter; a pad's Accept is its bottom face button (Xbox A / PS ✕).
Input is routed to one assigned player, not whichever join prompt is visible.

1. Choose keyboard+pad, pad+keyboard or two pads before launching. Keyboard port
   1 reserves P1, port 2 reserves P2, and 0 disables keyboard play. Restart after
   changing assignment; pads fill the other available ports in connection order.
2. P1 navigates ACTION → RACE → MULTI. At P2 join, P2 presses Accept on their
   own device. Both players confirm their own car selections, then follow the
   original event/track screens. SOLO uses SINGLE and needs no P2 device.
3. If no P2 device exists, the journey must direct the player back to SINGLE
   or explain connection/assignment. One keyboard cannot currently drive both
   players. A raw PS2 prompt alone leaves Mac keyboard users at a dead end.

README gives the concrete mappings. F2 now opens Controls and player 2 during
play, listing the live SDL assignments, bottom face button labels and the
keyboard's actual Accept binding. An empty P2 port explains connecting a pad,
keyboard reassignment/restart and SINGLE, including why P1 Enter cannot join P2.
Reopen F2 after hot-plugging to refresh. Menu window titles and Settings mention
F2. Virtual-controller checks cover those assignments and unplugging, but the
native dialog still needs a hands-on visual check. Automatic context-sensitive
replacement of PS2 artwork remains unfinished.
