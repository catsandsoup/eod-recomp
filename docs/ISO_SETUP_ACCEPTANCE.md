# ISO setup acceptance — 8 November 2026

## Player success

The player launches the downloaded game, selects their already-unzipped North
American v1.00 ISO, and the app installs it without a separate disc-extraction
tool. Returning launches use the installed copy without needing the ISO again.
Failure, cancellation, or another importer must preserve a previous installation.
Unicode source, executable, and account-folder paths must work. The player owns
and supplies the disc; packages contain no disc data.

Evidence below establishes the importer and headless Windows launch/return
journey. It does **not** establish visual native-picker usability, hands-on play,
full-speed rendering, or physical audio acceptance.

## Situation and task

Windows previously lacked the Mac first-launch importer. The initial Windows
adaptation still depended on POSIX filesystem calls; runtime data reads also used
ANSI Windows file APIs. These would prevent a correct install or subsequent game
launch from an account path containing non-ASCII characters.

The tested development tree was based on commit
`1151f6a486f8733a59a90c61d54f38a634152494` with uncommitted integration changes.
The tested Windows executable SHA-256 was
`9aeeb6e5c163cd39832d4b0f253d45d5129ccd9852026fd21a790cb25775a386`.
This is an acceptance checkpoint, not an assertion that a later release binary
has the same digest; final package verification must record its own digest.

## Action

Both platforms use the same ISO9660 reader and transactional disc importer.
Windows uses its native wide-character file picker and progress dialog. Mac
native setup offers cancellation and processes events between copy chunks.
Explicit `--data`/`EOD_DATA` image sources are accepted as well as existing disc
folders. A folder holding exactly one ISO is also accepted. Compressed archives
produce a message to unzip the image first.

The importer verifies `SLUS_209.10` against 3,368,996 bytes and CRC-32 `75BB93EE`,
then copies that executable, `SYSTEM.CNF` when present, and the `DATA` tree.
Per-destination OS locks serialize writers. Unique temporary copies are removed
on failure or cancellation; an interrupted final swap restores the previous
installation on relaunch. Abandoned partial copies are reclaimed under the lock
on the next import. Source files remain read-only inputs.

The runtime uses UTF-8 paths internally. Windows arguments are converted from
the original UTF-16 command line once at entry. Host file reads use wide opens;
Windows discovery, listing, and directory creation use wide APIs and return
UTF-8 names. Config and memory-card operations use the same path convention.

Existing-wheel reference checked: [RoadTripAdventure-recomp first-launch
workflow](https://github.com/silentsudin/RoadTripAdventure-recomp/tree/0f6d3613e8e32ccda5305227a578b1fb659983a8)
at revision `0f6d3613e8e32ccda5305227a578b1fb659983a8`. The existing local ISO
reader/importer was retained and repaired; no separate extraction executable was
introduced.

## Verified results

### Importer and filesystem checks

- `iso9660_test` passed synthetic plain ISO/raw-sector images, multi-extent reads,
  wrong executable/version, missing DATA/source, invalid images, archive
  rejection, Unicode/read-only ISO input, competing imports, cancellation/retry,
  interrupted-swap recovery, abandoned-copy cleanup, and invalid destination
  parent handling.
- `macos_startup_test` passed headless validation without creating `NSApplication`.
- `eod_mc_test` passed real memory-card packet persistence across independent
  processes. Its standalone platform-mkdir stub matches POSIX `mkdir(0755)`.
- `host_file_test` passed on Mac. Its Windows CMake target also passed under
  CrossOver through CTest, exercising real host opens, Unicode Windows discovery,
  directory listing/creation, output-buffer truncation rejection, and config
  save/load preserving Unicode paths. The MinGW test target links its runtime
  statically and needs no separately supplied MinGW DLLs.
- The full Windows executable and setup executable compiled and linked with
  MinGW-w64. An explicit `shellapi.h` include fixed the declaration hidden by the
  production `WIN32_LEAN_AND_MEAN` build definition.

### Whole Windows game: first install and return

Host: Apple Silicon Mac running CrossOver 26.3, using the isolated
`EOD-import-unicode` Wine bottle. Only that bottle's `Local AppData` shell-folder
registry setting was changed. No native Windows hardware was tested.

The install destination was absent before the first launch. The test used all
three Unicode path boundaries:

- Executable folder: `Game Montréal 日本語`.
- Owner-supplied image: `Owner Montréal 日本語.iso`.
- Installed root:
  `C:\users\crossover\AppData\Local\Montréal 日本語\TDEODRecomp\disc`.

Both launches used `EOD_HEADLESS=1 EOD_MUTE=1`, `--headless --frames 300 --scale 1
--renderer software`, with file/core/memory-card logs enabled.

1. First launch additionally passed `--data` with the Unicode ISO path. It
   installed the disc, loaded the native game's executable and streaming files,
   reached the 300-frame exit, and returned exit code 0.
2. All **128 imported files / 1,939,523,956 bytes** matched the owner's original
   installed disc using SHA-256 for every file, not just the ELF.
3. The test ISO was renamed unavailable. The second launch passed **no `--data`**
   and had no `EOD_DATA` environment value. It discovered the same installed
   Unicode root, loaded the game, reached the 300-frame exit, and returned 0.
4. Every installed disc file retained its modification time across the returning
   launch, confirming that startup reused the installed data instead of copying
   it again.

Both logs begin with the Unicode `game root`, followed by the ELF segment load
and entry point `00100008`, and end with `reached 300 frames - exiting`.
Observed elapsed times were 13.822 seconds for first install/launch and 7.141
seconds for return. These are functional-test durations, **not** matched
performance measurements.

Local evidence, deliberately outside Git because it contains owner-derived data:

- Workspace `work/windows-iso-20261108/validate.py`: exact commands, isolated
  environment, SHA-256 comparison, unavailable-ISO return test, and mtime checks.
- `work/windows-iso-20261108/results.json`: executable digest and test results.
- `work/windows-iso-20261108/first.log` and `return.log`: complete game logs.
- `work/win-unicode-tests`: independently configured Windows CMake test tree;
  CTest `host_file` passed in 5.32 seconds before the full-game check.

The workspace here is
`/Users/USER/Documents/Codex/2026-11-08/conversation-log-the-teamwork-system-has`.
These local paths are reproduction evidence on the owner's machine; they are not
public download links.

## Final Windows package checkpoint

After the final protocol/content checks, roster/error layout, and portrait-source
changes, the complete Windows build with `BUILD_TESTING=ON` succeeded. Its Ninja
dry run then reported **no work to do**. The old corrupt dependency log was backed
up and rebuilt; no source was removed to repair build metadata.

A fresh final run of **all 40 Windows executable CTests passed**, through the Wine
emulator, in 34.64 seconds. This includes content/rules, actual Opus codec, state,
control/protocol, UI model, camera, battle, custom music, and UTF-8 filesystem
checks. Final logs/JUnit: `work/win64/wine-executable-tests-final.log` and `.xml`.
Test-only MinGW DLLs were supplied beside those test programs; they are excluded
from the release archive. Small test portability fixes added the Windows
`_putenv_s` equivalent, explicit standard headers, a UTF-8 path conversion, an
SDL-only assertion guard, and missing Windows Media Foundation test libraries.
The production session include order was corrected so the EE `R` macro is
defined after Windows networking headers.

Final local archive: `work/windows-release-final/eod-win64.zip` (12,084,697 bytes).

| Artifact | SHA-256 |
| --- | --- |
| ZIP | `fd1106239af266ea939d600921bda80765f49eac89260085042081b147b34337` |
| Packaged `eod.exe` | `0645207a362473bc513b62ad60fc44dcb9b3ddf77eddf307f5558fdb2421e013` |
| Packaged `eodsetup.exe` | `26758087ee68d1d267fb408bc383449ac2850d6369d916db13d919b632c37084` |

The archive contains exactly `eod.exe`, `eodsetup.exe`, the final `README.txt`, and
`licenses/Opus.txt` plus `licenses/DearImGui.txt`. ZIP integrity validation passed.
Both shipped executables import only Windows system DLLs: no separate Opus,
libssp, libwinpthread, libgcc, or libstdc++ DLL is needed. The pre-strip game
contains actual `opus_encode`, `opus_decode`, `opus_encoder_create`, and
`opus_decoder_create` implementations, and its voice translation unit was built
with `EOD_HAVE_OPUS`. `work/windows-release-final/package.json` records the complete
file manifest, digests, and imported-DLL lists.

The preceding **exact ISO-acceptance archive** (ZIP SHA-256
`0f90c60f4b9321e728d033cc04d6636d151d1ad20a9663da312306fda941e102`, game
SHA-256 `b8f5d67397e6fcd5d47a231e8100a5efa846948e9b7f02c259785d50242ce68e`)
was extracted into a fresh Unicode game folder and tested again in an independent
`EOD-import-final` bottle. The fixture initialized only
the redirected Unicode Local AppData parent, as a real user profile would; the
app's disc installation started absent. First install/launch returned 0 at 300
frames in 10.224 seconds. The ISO was renamed unavailable, and a launch with no
`--data` returned 0 at 300 frames in 8.381 seconds. Both used the Unicode installed
root. All 128 imported files / 1,939,523,956 bytes matched the owner source by
SHA-256, and all imported-file modification times remained unchanged on return.
Evidence: `work/windows-release-final/acceptance/validate.py`, `results.json`,
`first.log`, and `return.log`. These are functional durations, not benchmarks.

After that acceptance run, the UI added a retail-UICAR portrait-identity whitelist
so custom livery swatches are not presented as portraits. The affected Windows UI
sources were rebuilt, linked, and confirmed clean by Ninja's no-work dry run; the
current archive/digests above include that change. The same 40 unit-test
executables are unchanged. The ISO journey was not repeated for this UI-only
change. The tested preceding archive and its manifest are retained at
`work/windows-release-final/acceptance/eod-win64-iso-checkpoint.zip` and
`package-iso-checkpoint.json`, so the recorded acceptance evidence remains
reproducible.

The large ISO fixture was built from the owner's already-installed disc files.
This proves realistic-file import and byte identity; it does not establish every
independently dumped retail image's layout or behavior on native Windows.
No release tag or upload was performed by this workflow.

## Remaining acceptance and next decisive tests

**Native chooser interaction remains unexercised in Wine.** A chooser-only
harness using the exact `windows_prepare_disc` implementation was launched, but
the computer-use provider exposed no Wine application/window and rejected
`getApp("eod-picker-test.exe")` as an invalid app. The test process was stopped;
no successful picker selection or native progress-cancel interaction is claimed.
Synthetic importer cancellation passes, but it does not prove that a player can
operate those dialog controls.

The decisive follow-up is a clean packaged-app run on native Windows and Mac:
select the owner ISO with the visible picker, observe progress, cancel and retry,
exercise a bad image and recovery, then relaunch after moving the ISO. Verify
actual memory-card saving and returning play. The 300-frame checks above reach
startup/streaming and do not establish race play, save creation, input latency,
rendering fidelity, or full-speed acceptance.

The final Windows package checkpoint above supersedes the earlier development
binary digest for release testing. Native Windows/Mac picker interaction and
Mac–Windows online interoperability remain separate acceptance journeys; report
their results independently.
