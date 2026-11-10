# Voice, music, and shared rules acceptance — 2026-11-08

## Player success and acceptance limits

Success means a player explicitly enables their microphone, another player hears
that captured microphone through their output, and silence/resume, mute/deafen,
permission/device failures and room changes recover without restarting. Microphones
start off. Music volume controls stock/custom songs locally while engines,
effects, spotter and voice retain their contributions. Online optional advantages
come exclusively from the host's shared rules; saved personal settings cannot win.

The evidence below establishes different portions of those experiences. It does
**not** establish native Windows microphone/speaker hardware, WAN audio, external
microphone unplug/reconnect, a fresh packaged-app privacy prompt, or an independent
acoustic recording of the actual game's remote speaker output. Those remain
acceptance gaps; software test counts do not close them.

## Verified software and physical evidence

Evidence files are local, work-only, and not shipped. Their root is
`$HOME/Documents/Codex/2026-11-08/conversation-log-the-teamwork-system-has/work/`.
No microphone recordings were uploaded.

| Scenario | Evidence and observed result |
| --- | --- |
| Codec/jitter/session | `netplay_voice_test`: real Opus encode/decode, ordered/reordered/lost/invalid packets, silence recovery, mute/deafen, all 16 player IDs including sender 15 and 15 concurrent remote decoders. No-Opus fallback also passed. |
| Actual two-process UDP to remote mixer, synthetic source | `voice-transport-20261108-225148/results.json`: host `127.0.0.1:27015`, guest `127.0.0.1:61770`; host received 449,280 mono samples, RMS 1409.13, dominant 655 Hz; guest received 455,177 samples, RMS 1197.41, dominant 450 Hz. Remote/local frequency-band power ratios were 301.91 and 522.86. This proves transport and mixer delivery, not hardware capture. |
| Physical acoustic roundtrip, work-only SDL probe | `physical-audio/probe.log` and `physical-results.json`: real **MacBook Pro Microphone** → Opus → real loopback UDP `127.0.0.1:55874` → decoder → delayed real **MacBook Pro Speakers** replay → real microphone. 428 microphone packets sent, 406 decoded, 410,880 captured microphone samples. The source goes only to speakers; encoded samples exclusively come from the recording device. A delayed 700 Hz replay was acoustically recorded after the original source stopped: tone power 25.38 versus baseline 4.77 and other-band 1.20. This is physical library/hardware proof, separate from game acceptance. |
| Actual game host microphone to guest mixer | `physical-audio/game-host-230332/results.json`: no synthetic capture hook; 365,210 received samples, 700 Hz source power 2622.75 versus baseline 0.384. Logs show built-in SDL recording/playback devices and granted AVFoundation permission. |
| Actual game guest microphone to host mixer | `physical-audio/game-guest-230441/results.json`: no synthetic capture hook; 331,621 received samples, RMS 392.85, 700 Hz source power 2276.94 versus baseline 0.259. |
| Music isolation in the actual SPU2 mixer | `music_volume_test`: stock and custom music at zero/half/full volume; same-stream speech and other-voice effects unchanged; stale song classification removed on RAM reuse. `custom_music_test` also passed song-tag bounds, partial-write/wrap/reset and its existing decode/library/shuffle tests. These use controlled signals, not a physical gameplay listening session. |
| Host rule authority | `netplay_rules_test`: all 32 advertised rule masks override both values of saved local preferences; online scales ignore personal finite/NaN/infinite values; offline settings return after leaving. All 256 low-byte native cheat remnants tested across turbo/chicken combinations; unsupported rain/vengeance/hardcore/master remnants cannot bypass all-off, while Battle retains its native weapon. |

Physical tests intentionally used visible windows and real audio because the
owner explicitly requested hardware evidence. Normal automated validation remains
headless and muted. Device defaults were built-in input/output, 48 kHz, system
output 44%, input 59%, output unmuted; the probes did not change those settings.
The first game analysis selected an unrelated loud menu transient. Corrected
analysis uses the known 700 Hz source window and a pre-source baseline increase,
so louder unrelated game sounds cannot stand in for microphone transport proof.

## Actual-game acoustic output follow-up

A work-only receiver playback shim delayed actual AudioQueue output by three
seconds without changing microphone capture, encoding, UDP or mixing. An
independent physical microphone recorded the original speaker source and a later
700 Hz plateau after the original source stopped. Evidence is
`physical-audio/game-host-231653/`: `guest/game.log` identifies actual 48 kHz stereo
MacBook Pro Speakers output and the 576,000-byte delay FIFO;
`independent-mic.raw` and `acoustic-windows.json` preserve the recording/analysis.
Actual game remote PCM again contained 700 Hz (539,212 received samples; source
power 9556.7 versus baseline 2.51).

This does **not** close the independent game-speaker acceptance gap. Delayed
physical tone power reached 96.0, but competing nearby game-audio power was 56.9,
a ratio of 1.69 rather than the chosen 4:1 isolation criterion. Menu/race sound
masked the replay. The post-source baseline window in the first analysis also
included the tail of the original source, so that analysis was rejected. No
successful acoustic acceptance artifact was written. This is supportive evidence,
not a substituted passing result. The isolated staged hardware roundtrip above
remains the clean physical speaker proof; an actual-game rerun with quieter
native game sound and independently separated acoustic source/output is still
required. All delay/probe code remains work-only and is not shipped.

## Implementation and source review

The speaking indicator previously gated encoding. Silence stopped sender sequence
advancement while receiver playout advanced, so resumed packets became stale.
Encoding now follows capture cadence; the indicator is separate. Bounded jitter
starvation resets and re-primes playback. Microphone start remains opt-in/default
off. Capacity is 16, aligned with the room protocol.

On macOS, an explicit microphone action checks AVFoundation authorization before
opening SDL capture. Not-determined status requests native OS permission; denied
or restricted status leaves capture closed. No privacy database edits or automatic
OS approval occurred. SDL logs real device names, validates resume results, and
returns capture failures to the session, which closes the stream and retries after
three seconds. Actual external-device loss and denied/restricted prompt journeys
still require physical/user acceptance.

Stock song blocks are classified from existing music-bank records. SPU RAM writes
and initialization invalidate classification; stream refills retag after writing.
Only classified stock stream samples and host custom-music PCM receive music gain.
Voice is mixed separately. The playlist and volume remain local.

Review of `game_patches.cpp`, `hle_recovery.cpp` and `battle_driver.cpp` found the
optional turbo, chickens, invincibility, durability and manual recovery paths use
`netplay_hooks::rule`; turbo and chicken multipliers use `race_scale` with shared
values. Both host and guest disable local mod combos. The review also found
unsupported native rain/AI/hardcore/master flags could survive offline-to-online
transition. Online native-word normalization now clears those remnants and derives
master/turbo/chicken enablement from shared rules, preserving Battle's native
weapon. Personal career-cheat preference no longer enables online master cheats.
This is source review plus focused helper tests, not a full native-game matrix for
every modifier or a mod-file compatibility proof.

Upstream behavior reused: [official Opus decoder documentation](https://www.opus-codec.org/docs/opus_api-1.6/group__opus__decoder.html)
for ordered stateful decode, null-packet concealment and reset;
[Apple capture authorization documentation](https://developer.apple.com/documentation/AVFoundation/AVCaptureDevice/authorizationStatus%28for%3A%29)
for permission-state checks before requesting access.

## STAR handoff and next decisive checks

**Situation:** local microphone activity alone had not established remote audio;
stock music lacked an independent local volume, and room rules could inherit
personal native cheat flags.

**Task:** establish reviewable software and physical evidence, isolate music gain,
and make host rules authoritative.

**Action:** repaired capture/sequence behavior and jitter recovery, expanded voice
IDs, added permission/device diagnostics and retry, tagged native song blocks,
added real mixer and rule-authority tests, and ran the separately identified
synthetic, actual-game physical-capture, and staged acoustic probes above.

**Result:** the listed test/probe outcomes passed. Root owns final current-source
build, test registration/full-suite checks, packaging/install, and release. The
remaining decisive tests are fresh packaged-app permission/denial/retry, actual
game acoustic remote-output capture, native Mac ↔ Windows microphones/speakers,
external microphone loss/reconnect, WAN voice, and physical mute/deafen journeys.
