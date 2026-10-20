# Personal Mac build with on-demand mods

## Player success

Open one branded app, choose Native, Phantom, Battle or Phantom + Battle, then
play using the installed disc. Change implemented gameplay options with F1;
restart to change resource profiles. No terminal commands are required to choose
a packaged profile. Preserve the disc, saves, personal settings and source work
while deleting superseded executables and merged development branches.

Acceptance requires the signed app to contain all three local profiles, startup
selection to run before resource streaming, and a Combined profile that retains
both car registration and Battle definitions. Headless regression and real game
journeys establish resource compatibility; they do not establish visible dialog
interaction, driving feel or exhaustive compatibility across every event.

## How to play

- Native uses the retail resource catalog, with gameplay settings applied.
- Phantom adds the cloned native car with its custom livery, menu image,
  registration, Career description and performance values.
- Battle uses the optional fourteen-map catalog. Action offers native race plus
  Battle; multiplayer offers Battle. Career definitions remain.
- Combined merges Phantom and Battle against the original COMMON.RES, rather
  than overwriting one mod's shared archive with the other.

The chooser remembers the previous selection. F1 exposes chicken weapons, turbo,
invincibility, Career cheats, manual recovery, damage scaling, Battle bots, unlocks
and turbo/chicken multipliers. Existing renderer, detail and aspect controls also
remain available. Battle bots apply at the next event; detail/aspect changes
apply next launch. Invincibility protects **all cars**, so disable it for ordinary
Battle damage kills. Chickens and turbo share L2 when both are enabled. F2 shows
controls. The original 100% speed indicator is not proof of smooth drawn FPS.

Profiles are resolved from `Application Support/TDEODRecomp/mods/<profile>`;
if that profile directory is absent, the app uses `Contents/Resources/Mods`.
An explicit EOD_MOD_DIR overrides the chooser. Headless runs never show it and
do not implicitly select a remembered graphical-session profile.

## Build and reuse

Reuse the existing native Cocoa settings dialog and explicit read-only resource
overlay (see RENDERER_REFERENCES.md). No new mod loader or renderer is needed.
Generate Phantom and Battle with the existing source tools; compose them with
`tools/build_mod_profiles.py`. The composer's three-way merge refuses different
edits to the same resource entry. It publishes only a complete new directory.

For a personal app, set EOD_BUNDLE_MODS to a directory containing `phantom`,
`battle`, and `combined` when running the existing CMake Mac app build. Normal
source builds bundle no profiles. Generated archives derive from the owner's
disc: keep them local and out of Git and public releases.

## Known limits

Battle retains the native four-car limit. AI navigation can stall at obstacles.
Imported track geometry, arbitrary replacement vehicle meshes, expanded racer
capacity and NASCAR conversion are unfinished and are not offered as supported
profiles. Existing native geometry and physics remain. High-resolution collision
and effects slowdowns remain open; see COLLISION_PERFORMANCE.md.

## STAR handoff

**Situation:** separate launch scripts selected mutually overlapping resource
archives, and obsolete builds/merged checkouts made the current app ambiguous.

**Task:** one current app with explicit profile selection and implemented live
settings, retaining unfinished work and recovery evidence during cleanup.

**Action:** extend the existing Cocoa startup/settings flow, validate profile
presence before selection, merge archives against the original, and optionally
bundle personal profiles before signing. Record cleanup manifests and a Git
bundle under ignored `work/cleanup-2026-10-20` before deleting old builds.

**Result:** see DEVELOPER_HANDOFF.md for the actual verification results. Visible
chooser/F1 interactions still require hands-on player acceptance; automated
headless tests deliberately do not open game UI. The next decisive performance
test remains a repeatable collision/effects workload at the player's resolution.
