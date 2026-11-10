# Anoka environment editing and native playtest

Player success: open Anoka's actual environmental assets, preview and save an obvious sky change to a separate native overlay, reopen it, and drive the saved result in the actual game. The accepted Modding Suite, original game binary, user projects and extracted disc remain unchanged. Development startup should hide Atari/FMV/loading presentation until a real race frame is displayed; resource loading still runs.

## Verified environment workflow

Anoka's `track.grf` references32 sky tiles: `sky01A.tex`/`sky01B.tex` through `sky16A.tex`/`sky16B.tex`. The native GRAF Sky record dispatches through constructor0x221E00 and draw0x223858 to0x34BDA8. These are actual existing sky resources, not the proposed `.hdr` schema from earlier research.

The development Kit exposes track texture inspection, PNG export, same-size indexed PNG replacement and whole-sky palette RGB multipliers. Preview prepares edits without writing the mod; Save & Build publishes native texture bytes plus source/output hashes in an isolated project. Reopening uses the saved edits. Run Mod in Game chooses the project's saved track and car, not a hardcoded Anoka target for every project. All texture sources and their existing metadata/layout are retained. Model viewing, native table editing and native export remain available.

Example project (ignored local content): `mods/AnokaEnvironment/project.eodproject`. It contains a deliberately conspicuous purple sky, using RGB multipliers8,1,8 across32 native palettes. Every unedited native member remains byte-identical, including `track.grf`, collision and AI lines. For same-size texture edits, the entire archive is preserved except edited payload bytes and their directory CRCs. The retail `roadrace.ili` already has a CRC discrepancy; its original bytes and directory entry are preserved and the discrepancy is recorded in sourceCrcWarnings, not silently repaired. Edited source members must still pass CRC checks.

PNG import supports native4/8-bit indexed TEX with representable upload layout and the same dimensions. Noninterlaced8-bit RGBA PNG is required. Existing palette capacity16/256 means quantization can be lossy; regenerated mips use nearest-neighbor filtering. Alpha and native header/layout are retained. Direct-color editing, HDR lighting, arbitrary geometry/track/collision compilation and live3D world placement remain unimplemented. Native race capture is labelled as a capture; the playable world runs in the actual separate game.

## Native startup hooks and evidence

Verified SLUS_209.10-specific hooks:

-0x00113FF8: generic session-state draw dispatcher. Race loop caller0x1140E0 establishes draw scope; this function alone is NOT readiness (it also shows loading).
-0x00106338: state-zero gameplay viewport world/HUD callback from table0x3C3CF8+0x0C; calls world/HUD builders0x101618/0x101C00. Valid viewport count is checked before considering it a gameplay draw.
-0x0033EC78: native FrameWait completion, caller0x11412C, includes0x35C3F8 packet/display publication. The ordered GS marker follows this work.

Startup is opt-in with EOD_EDITOR_TEST=1 plus EOD_DIRECT_START. It skips FMV playback and creates a hidden SDL window. Readiness must follow a fresh composition of a subsequent actual displayed-buffer draw, not a generic draw call, register-only change, stale redraw or menu overlay composition. Window operations stay on the main thread; hardware/software presentation acknowledgements are separate. EOD_EDITOR_PREVIEW optionally writes one atomic PNG of the eligible native scanout through existing screenshot code. Headless evidence explicitly has no window/presentation acceptance.

Early acceptance attempts correctly FAILED visual inspection: first capture was still the loading image despite the initial generic-draw and first gameplay/publication gates. Those were insufficient; tests passing did not override the captured evidence. Final native-image/visible-window status must be recorded below before any completion claim.

## Evidence and handoff

Ignored local `work/track-environment/` holds native baseline/modified captures, logs, tests and actual project hashes. Baseline and edited2400-frame muted/headless runs both exited0 with throttle input; the car moved to the outside barrier. Actual image comparison shows the purple sky and intact world. Every non-sky payload and the original archive is unchanged.

Kit integration:7 suites pass including real owner-supplied texture/model checks and two Swift production-handler save/reopen tests. Review found a persisted failed selection and unguarded discard handler; regression tests reproduced them and fixes preserve a valid edited selection and guard pending operations. The inspector already inherits busy-state disabling; the explicit handler guard also protects programmatic calls.

Initial full runtime suite85/87: AssetTools failed because CMake selected a Python3.14 without Tk; SymbolsDB failed because newly added hooks lacked names. Use the existing Python3.13 installation (no new dependency), add verified symbols and rerun. Do not omit these failures from the record.

STAR: Situation—vehicle-only browser did not supply world/environment editing. Task—one real saved track environment workflow and a separately proven direct-race presentation. Action—native resource investigation, layout-preserving sky patching, editable PNG pipeline, track-aware launcher and opt-in runtime handshake. Result—environment edit/save/reopen/native play verified; final startup presentation acceptance is separately recorded after image inspection. Working app/game/disc protected. Next work after this milestone is complete native world assembly/placement, not a claim that all formats or modern rendering are supported.


## Final executed result

The native fade field is confirmed opacity, not generic flags:0x175960 stores0x176278's result at world+0x1964;0x210FB8 returns it;0x113EE8 skips drawing the0x34E198 full-screen alpha rectangle only at zero. Final eligibility requires opacity0 during actual gameplay world/HUD draw, later native frame publication, a subsequent displayed-buffer draw, composition and (for a visible window) successful presentation. No clock/frame delay is used. Regression rejects255 and residual1.

Final actual retail capture: `work/track-environment/direct-race-fade-clear/first-race.png`, native vblank143/drawn112, visibly shows the bright playable race, purple edited sky and25MPH HUD, without loading/FMV/Atari imagery.300-frame run exited0. This proves the headless first-eligible native image; no visible SDL window was created, so it is not evidence that the hidden-window/Metal presentation path was observed on desktop.

Desktop UI inspection could not be completed: Computer Use could not attach the development app, and Finder reported concurrent user state changes. No working app was changed to force a GUI test. Production Swift handler tests cover track selection/preview/save/reopen, and the signed app is available for direct inspection.

Initial full runtime85/87 failures were corrected by selecting installed Python3.13 with Tk and adding verified native hook names; rerunning those2 tests passed. Final full runtime run:87/87 tests passed in52.53 seconds; exact output is in ignored runtime-tests-final.log. Kit7/7 suites and Swift2/2 production integration tests passed. Native input acceptance is bounded to one Anoka race; this is not all modes/vehicle types or HDR/PBR support.
