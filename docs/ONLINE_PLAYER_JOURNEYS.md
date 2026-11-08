# Online player journeys

This is the player contract for the 1.0 release. “Implemented” describes code. “Exercised” means the stated scenario ran. “Proposed” identifies design work. A passing localhost test is evidence for its specific player count, event and exit path.

## Seats, cars and voice

| Question | Current behavior | Product decision or acceptance still needed |
|---|---|---|
| How many can join? | The protocol assigns IDs 0–15. The host occupies 0 and can admit up to 15 guests. A full room refuses a new connection. | This is an admission ceiling. Only two simultaneous humans have exercised the traced rematch. Test 3, 4, 5, event capacity, 16, and a 17th connection. |
| Can I customize lobby size? | The host sets MAX PLAYERS from 1 to 16, including themselves. It cannot shrink below current occupancy. Independent AI count, laps, reservations and access control remain separate work. | Actual UDP admission tested at 16, overflow refused, clamp at occupancy, reduction to 15 and replacement admission after restoring 16. |
| How many humans can race? | Each machine supplies one locally driven car. Human-driver and pad-zero bindings are validated before GO; initial grid poses are acknowledged before simulation. Remote drivers occupy eligible AI seats in the selected event. The roster reader accepts 1–16 total cars. Battle construction caps **all** cars at 10, including humans and AI. Event choice and available AI seats can lower the human limit. | Preflight the actual grid before Start and show a reason if anyone lacks a seat. Do not promise a universal 16-human race. Define AI displacement or lobby-only seats before advertising larger rooms. |
| Who can speak? | The Opus session handles all player IDs 0–15. | Codec tests mix 15 remote streams including sender 15. Physical microphone and acoustic evidence are separated in VOICE_AND_MUSIC_ACCEPTANCE.md. |
| Can I join during a race? | The host refuses a fresh join while its event is active. | Show “Race in progress” and allow retry. A waiting room, spectating, and guaranteed seat reservation are proposed. |
| Can we mix local split screen and online? | The online setup selects single local player per machine. Local split screen is a separate game path. | Mixed local/online seats are outside this release. |

Evidence in source: [wire capacity](../src/netplay_wire.h), [admission and roster assignment](../src/netplay_session.cpp), [Battle grid construction](../src/battle_bots.cpp), [Battle total-car bound](../src/battle_layout.h), [voice peer limit](../src/netplay_voice.h).

## Host and guest actions

| State / player job | Expected response | Current evidence or open decision |
|---|---|---|
| Offline: choose online or stay local | Host, Join by Code, Join by Address and discovered games. Back leaves local play intact. | Offline Online-tab commands have UI tests. |
| Host: create once, share access | Allocate a code and address after the socket opens. Keep code visible when scrolling. Show roster, selected cars, available slots and voice state. Hide Host/Join. | Implemented code, address, persistent roster pane and speaking indicators. Room-size selector implemented. A full roster uses two columns; the code and error reason remain above it. |
| Guest: browse, paste a code, join or cancel | Text editing, invalid codes, unavailable host, incompatible version and timeout have distinct outcomes. Cancel returns to browsing. | Implemented network paths. Test internet/NAT, malformed inputs and cancellation on Windows. |
| Joining: repeat an action | One connection attempt. Never replace a live host or guest session by accidentally selecting another lobby. | UI hides conflicting actions; command guards reject them. |
| Lobby: choose car and paint | Each player controls their own choice. Other players see the car. A guest changing it must ready again. | Trailer2 was exercised twice on each peer. Paint choice is local selection; remote roster currently communicates the vehicle name, not a distinct paint label. |
| Host: choose track and rules | Guests can inspect track/event. Only host can change them; freeze choices for loading and racing. | Track/event and shared turbo, Chicken Fight, invincibility, durability and recovery controls implemented. Defaults are all off, even with offline cheats saved on. Native rain/AI/master cheat remnants are cleared online. Independent laps and AI count remain proposed. |
| Guest: ready / unready | Show whether settings or a guest are holding up Start. Host can start alone. | Readiness now gates Start; unavailable Start explains that the party or game data is still pending. More specific per-player wait reasons are proposed. |
| Party: establish content parity | Verify gameplay content before Ready. Identify the missing car or track and offer a mutually playable selection. | JOIN/WELCOME require matching effective DATA compatibility fingerprints, including selected overlay archives and Battle alternate layouts. Audio banks, movies and external custom songs are excluded. Missing selected vehicles abort instead of substituting a car. Same-size archive differences and mismatched actual UDP admission tested. Automatic mod transfer is absent. |
| Host: start once | Start locks selected event, car and paint. All clients enter the same numbered event without navigating Career. | Two-player localhost Anoka / Trailer2 rematch exercised. Late, duplicate and mismatched packets need impairment tests. |
| Loading: cancel / one peer fails | Host can cancel the start. Guests can leave. Keep the lobby identity visible and show which peer or resource is pending. | Cancel path exists. Loss during loading, stuck loaders, bounded recovery and returning all peers after partial admission need dedicated tests. |
| Racing: drive, pause, browse controls | The live overlay shows Racing, participant cars, code and voice; race choices are read-only. Overlay input must not freeze the simulation or leak into native menus. | Phase-driven controls implemented. Visible input/focus, controller reconnect, Esc/Start and opening every tab need hands-on tests. |
| Racing: hear a friend, mute mic, mute others, deafen | Show speaking identity, capture availability and mute state. Transmit only voice. Recover from denied permission or a removed device without stalling the game. | Both Mac roles captured the built-in microphone into the remote game mixer. A separate acoustic mic→codec→UDP→speaker→mic roundtrip passed. Actual-game speaker output, native Windows hardware, per-player mute, denied permission and external-device loss remain distinct acceptance gaps. |
| Either player: play my own music | Each device chooses and plays its own soundtrack at its own volume. A guest's music files should never change the host's playback or eligibility. | Custom music is local; a two-device audible test with different playlists is outstanding. |
| Host: end the race for everyone | Return through native cleanup, keep the party and code, then focus Online. Clear the banner only when cleanup completes. | Both peers returned to the lobby on two consecutive host-ended races. |
| Natural finish / results | Show who won and which scoring source is authoritative. Return to the same party and support rematch. | Native cleanup changed. Natural completion for each event type and host-authoritative results/damage remain acceptance gaps. |
| Host: change settings, rematch | Retain members and cars. Let guests acknowledge readiness again. Start a fresh event number; ignore stale messages. | Two starts and two returns exercised. Changing to another track for the second race remains to test. |
| Guest: leave race / party | Leave Match currently disconnects this guest. Others keep racing. Waiting out a round in the party needs a separate action and seat policy. | Command path exists. Three-peer continuity, slot reuse, late snapshots and reconnect need testing. |
| Host: kick a player | Select a named guest, confirm, give them the reason, free only their seat and preserve the rest of the party. | Controller action requires a second confirmation; Back cancels. Confirmation carries occupant generation, so slot reuse cannot remove a later joiner. Actual UDP test removed only one of 15 guests; remaining 14 stayed connected. Full three-game-process race removal remains acceptance work. |
| Host: close lobby or quit app | Explicit close informs guests. Unexpected loss times out, unwinds the race, shows the reason and allows browsing. | Abrupt host termination exercised during the race: guest reached offline at native main menu in about 5.6 s. Test graceful close during each phase. No host migration is promised. |
| Returning: native game does not exit | Display the wait reason and elapsed time, keep an exit action reachable, and offer bounded recovery. Preserve diagnostics and saves. | Trace and stale End-message handling implemented. Proposed escalation after 5 s and a bounded restart path still need implementation and stuck-menu injection tests. |
| Disconnected: reconnect, host or play locally | Forget stale event, seat, remote-car, voice and code state; rejoin through discovery or code. | Test rapid rehost, a different host with lower event numbers, peer loss during each transition and return to offline Career. |
| Player needs accessible controls | Reach every action with gamepad and keyboard. Keep focus stable as peers join or leave; don't rely on color alone. Provide readable code, car, role, ready and voice labels. | Keyboard menu navigation is tested. Window sizes, focus retention, color/contrast, long names and controller-only text entry remain acceptance items. |

## Trace and performance acceptance

Use `EOD_NETPLAY_LIFECYCLE=1` to log role, event number, native main-menu presence and phase changes. The changed race path is lobby → loading → racing → returning → lobby. If the host disappears, the terminal state is offline. Capture both peers with monotonic timestamps, including chosen track/car, command, reason and native cleanup. Export only the relevant records and omit access codes.

Run `tools/test_online_rematch.py EOD_BINARY DISC_FOLDER NEW_LOG_DIRECTORY` with the integrated profile available. The test obtains the actual host port, runs two headless muted processes, and requires two Trailer2 starts/returns without a Career diversion. Test the same journeys with real controllers and render output before claiming visible parity.

For capacity and performance, hold track, event, car, AI, render scale, frame skipping, camera, inputs and workload constant. Measure solo offline, host alone, host + one guest, four peers, and occupancy up to the selected event's actual seat ceiling. Record simulation rate, drawn frames, p50/p95/p99 frame time, input delay, snapshot age and GS/EE waits. Report voice off and on separately. Add packet delay, jitter, loss, duplication and host process termination. A visible 19 FPS observation remains open until reproduced and fixed on the affected path.

For Mac/Windows interoperability, test native Mac ↔ Wine-hosted Windows in both roles, then native Windows hardware. Use separate saves/configuration and the same content. Exercise join, ready, start, steer, results, return, rematch, voice and disconnect. Run each performance benchmark alone as well as the combined compatibility test; two copies on one Mac compete for the same CPU and GPU.

## Two people on the couch, one keyboard exception

Use two independent clients and saves. One player hosts and reads the Garage Code aloud. The other opens Online with the controller, chooses Join by Code, types only the five-character code, puts the keyboard down and presses Cross/A to confirm. Circle/B cancels text entry. Each player chooses their car and paint with left/right; the guest marks Ready; the host starts. Both see the other person's car and progress without reading the other screen. During a race, Start opens Online, and Back to Lobby keeps the party together. The guest readies again for the rematch. A LAN or remembered-friend join needs no typing at all when that listing is available.

| Couch usability gate | How to exercise it on both screens |
|---|---|
| Find Online and discover the invite | Start + Select opens the menu; L1/R1 cycle tabs, D-pad navigates, Cross/A confirms, Circle/B backs out. Code and roster stay visible while the host highlights settings or voice. Validate glyphs change back to pad after typing. |
| Recognize what changed | Have the guest change cars while the host is focused on the microphone. Neither player's selection should jump to Disconnect when the roster changes. Host must see the new car and cleared readiness. |
| Make the common action easy | Track, car, paint, Ready and Start precede disconnect. Read-only information does not require navigating through every player row. Starting/returning/racing should never share misleading “Starting event” copy. Count presses for host, join, return and rematch. |
| Avoid expensive mistakes | No Host or Join during an existing session. Explicitly distinguish End Race for everyone, Close Lobby for everyone, Leave Match for me and Back to gameplay. Confirmation for closing/removing a player and recovery for accidental Back remain design work. |
| Keep the keyboard exception narrow | A mistyped code must be cancellable with B and resubmittable with A. Test paste and case handling, full/unavailable lobby, a wrong code and returning focus after an error. Manual address entry is an advanced alternative. |
| Respect effort over repeated races | Retain choices after return; the lobby shows what is waiting and why. Long car lists need a browsable grid or favorites; a linear left/right picker alone gets tiring. Measure time to find Trailer2 twice and a different class on rematch. |
| Watch and listen | Test one real mic on each end, simultaneous speech, mute, deafen, unplug, denied permission and two different playlists. Both screens must show the correct speaking identity. |
| Recover without expert knowledge | Unplug the host connection while racing; the guest sees the reason and reaches offline browsing. Inject delay/loss, cancel during loading, retry a different code, and check no previous match or code reappears. |

These gates apply Nielsen Norman Group's heuristics on visible system status, recognition, user control, error prevention and recovery to specific game actions. [Usability heuristics](https://www.nngroup.com/articles/ten-usability-heuristics/)

**Verification boundary:** the localhost admission probe obtained all 15 guest IDs from an actual host; the next join received the full-room response. These were protocol clients, not 15 simultaneous drivers. Two real game processes exercised rematches. Headless automation, including simulated controller input, does not establish couch comfort, visible latency, accessibility or physical controller ergonomics.

## Cross-platform results on the M2 Pro

CrossOver 26.3.0 ran the x64 Windows game. With the native Mac build hosting, the Windows guest joined over loopback, loaded Anoka twice, raced twice and returned twice. The reverse pairing also passed with Windows hosting and Mac joining. These were two real executable instances using the same integrated content, isolated configuration and memory cards.

One separate single-instance comparison used the same Anoka No-Rules Race, Trailer2, 1× render scale, 4:3, no frame skipping, muted/headless, a 2,000-vblank cap and no competing game instance. Native Mac finished in 33.5 s. Windows through CrossOver finished in 36.7 s including its launcher. Native reported 54.0 drawn FPS in the first five-second sample and 59.9 thereafter. CrossOver reported 36.0 initially, four samples at 59.9, and 50.8 in its last complete sample. These are interval averages. Visible presentation, input-to-photon latency, p99 frame times, internet conditions and native Windows GPU performance were not measured.

CrossOver's Direct3D translation backend and the Mac's GPU are part of that result. [CrossOver graphics backend documentation](https://support.codeweavers.com/en_US/advanced-settings-in-crossover-mac-26)

A separate two-process test drove the host through the live menu using scripted pad input: Start + Select opened the overlay, L1 reached Online, Cross hosted, the D-pad navigated to Start, then Start and Cross ended the race. Both clients reached the lobby. The guest joined by loopback address and automatically readied in this test. The test profile disabled the offline menu pause so the vblank-based input script could advance. The guest typing only the code and confirming with the controller was tested in the menu model; real internet-code redemption and physical TV-distance usability remain open.

## 1.0 acceptance update

Player-control evidence now includes a long two-process race beyond the previously
reported 5,100-frame failure: native ticks reached 8,700 host / 9,000 guest, finite
terrain positions, continued remote snapshots and separate throttle/steer/brake
blocks. Identical Trailer2 choices occupied different native slots. A separate
standing Battle used Trailer2 and Moth, six distinct starting bodies, zero initial
velocities and independent controls. Initial 14-car grid center separation was at
least 5.248 m. Event-scoped snapshot IDs and monotonic remote timelines reject old
or reordered state. These are specific traces, not a claim of perfect netplay.

The real-session protocol probe exercised duplicate nicknames through (15), guest
and host renames, same-endpoint reconnect, capacity changes, targeted removal,
stale removal confirmation, forged guest rule updates, content mismatch and old
protocol refusal. A 16-person party could not start a 14-seat race: no GO was sent,
the native flow unwound to the lobby, and the error remained visible under the
pinned code. Those 15 guests were protocol clients, not 15 physical drivers.

The previous 19–20 FPS observation, WAN impairment, natural completion of every
event, host-authoritative final scoring/collisions, high-occupancy race performance,
controller ergonomics on both OSes, and native Windows audio remain open. Windows
through CrossOver establishes executable interoperability rather than native GPU
or peripheral parity. No host migration, spectator mode or mod download is claimed.

### STAR handoff

Situation: race ownership, reused body bindings, missing grid acknowledgement and
ambiguous lobby state could leave players in Career or with stale remote motion.
Task: make each admitted driver own one valid car, keep starts/returns coherent,
and keep party identity and failure recovery visible.
Action: resolve current native body arrays, verify human bindings, assign unique
start records, handshake the initial grid, scope snapshots to events, normalize
shared rules, check effective game content, add nickname/capacity/moderation UI,
and verify the specifically listed scenarios.
Result: the listed long race, standing start, rematch and protocol checks passed.
Remaining acceptance is recorded above and in INVESTIGATION_QUEUE.md. The next
broad confidence test is a real three-machine race with network impairment and
natural finish, followed by physical Mac/Windows input and audio.

The content check adapts the canonical overlay inventory and fail-closed type
handling from the repository's existing tools/netplay_content_manifest.py. Its
64-bit FNV checksum detects accidental compatibility differences; it is not
cryptographic authentication or protection against a malicious modified client.
Installed assets are assumed read-only during a session. Music and movie exclusion
keeps individual soundtrack choice independent of gameplay parity.

The final 1.0 packages repeated two-race rematches in both hosting directions;
see RELEASE_1_0_ACCEPTANCE.md for the final checkpoint.
