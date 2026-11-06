# Cup wheel import checkpoint — 2026-11-06

Player acceptance: Cupcar uses NASCAR tyres, rims, dimensions and texture data;
all native cars retain their wheels; no missing resources, wrong-handed wheels
or changed user profile. The completed checkpoint imports and verifies the
source generic slow and independent fast meshes used by the audited Cup gameplay
loader. The wider role of side-prefixed source assets remains unclassified;
those assets are not selected by that audited gameplay path.

## Native reader evidence

Disassembly is from owned SLUS_209.10 SHA256
`4f60053dc141365f5ae50737622f8408be30ced513b4c109427c926461f2e2ae`.

- `0021c3c8`, especially `21c400..4bc`, reads Front/Rear `wheel_class` and
  creates four instances via `2216b0`→`221510`.
- `0021f780`, `21f7b4..7e0`, formats `%s.val`; there is no enum restricting
  the key to native classes. It reads dimensions, lod, model and optional shadow.
- `21f830/850` multiply the diameter by0.0127 (inch→radius metres);
  `21f888` multiplies width by0.0254. ncu.val translates source305mm to inches.
- `0021f240` scans up to eight lodN entries, parses `%f %s`, squares the
  distance and tests for `y` in the rotation flag.
- `0021f3a0`, `21f418..428`, formats only `%s%d.mod` from model.base and
  LOD. `21f438..470` handles a missing LOD0 by falling back to1.
- `21f478..594` applies slow_remap; `21f5a0..6a4` applies fast_remap to
  textures in the same model (`34b720`→`34ca70`). `21f6b8..6e4` obtains
  two handles for that model, stored at class+28/+2c plus16*LOD. There is no
  source-filename branch for independent fast geometry in the original loader.
  The completed scoped C++ extension below supplies that missing selection.
- `00220788`, `2207b8..7d4`, tests wheel index0/2; `220858..864`
  reverses rotation for the opposite side. This is transform handedness;
  both sides share the class mesh. Front and Rear can choose different classes,
  but native data alone does not assign different left/right meshes per axle.
- `0021f988` allocates0x5a0 bytes and initializes eight0xb4-byte class records.
  The append paths at2213c8 and2215c0 have no capacity check. This is eight
  **concurrent classes**, not eight VALT files on disc. The tested14-car race
  holds only `mid` and `ncu`. Future mixed-class/multiplayer acceptance must
  audit the concurrent count; arbitrary future profile combinations are unproven.

NASCAR SLUS_204.41 SHA256
`5ba427ff39f53343ac07cd67c870a0e5b61afdd14ec81aca54c943e137f297d2`:
`001d8f58`, `1d9168..174` separately formats `%s%d.mod`, and
`1d9178..194` formats `%s%df.mod`. This proves the source supports independent
fast geometry that EOD's original texture-remap path cannot reproduce exactly.
The implemented optional resource hook now selects those source fast meshes.

## Source conversion

`nascar_wheels.build_cup_wheels` reads fresh corrected-RST0 RACE.RES data.
It imports generic Rtire0..4 and independent Rtire0f..4f, keeping every decoded
position, rigid part type and material flag. These packets are rigid MSCAL26, matching retail EOD wheel
MSCAL55 (rather than body skin MSCAL2d→5c). Existing VIF normal and UV row
conversion is reused. TEX conversion changes the header and preserves pixels.
All names receive ncu prefix to avoid replacing shared retail Tire/Tread/BlurTire.

Rtire.tab is a TABL with5rows×2columns,0x4c header and33-byte cells.
Source thresholds5/10/20/40/240 and `rot` flags become native lodN values
and y/n flags. Source Front tire_diameter28, diameter17 and width305 provide
wheelmod dimensions. Source tyre handling remains intact on both source axles
and dirt copies; wheel_class=ncu is the EOD-only adapter.

No donor wheel shadow is retained: the native shadow section is optional and
NASCAR supplies no corresponding wheel shadow here. The body retains its
already source-generated shadow.

The final import includes generic Rtire0..4, independent Rtire0f..4f and
source blur textures. The source also contains RTire5 and the side-prefixed
Rltire0/Rrtire0. Rltire0/Rrtire0 have LTube5,
WHEEL2 and (right) Detail1 surfaces and are not equivalent to generic Rtire0;
their wider original role is unclassified. Source1d90c0 reads axle `model`,
then1d9168/1d9180 format that unchanged prefix plus LOD/f: Cup physics supplies
rtire on both axles, so the audited Cup gameplay path does not select those
side-prefixed resources. No speculative substitute is made.

## Reuse decision

Checked RoadTripAdventure-recomp main
`0f6d3613e8e32ccda5305227a578b1fb659983a8`:
https://github.com/silentsudin/RoadTripAdventure-recomp/tree/0f6d3613e8e32ccda5305227a578b1fb659983a8
Its PS2 recomp/runtime integration is useful upstream context, but it does not
establish this Monster Games wheelmod resource ABI. This change reuses the
recovered, native-tested MODL/VIF/TEX/RST0/VALT tools. The implementation adds
a scoped C++ fast-resource hook and an opt-in wheel-selection trace, documented
below. It preserves the native loader for ordinary classes and fallback cases.
No renderer or GPU implementation changes were made.

## Final optional fast geometry and runtime evidence

The focused hook wraps0034cef0 only for RA0021f6dc and exact ncu0..4.mod names.
It probes the corresponding ncuNf.mod through native001326f8, restores every EE
input register, loads once through the original and restores caller filename
bytes. Native classes, other callers and absent fast resources keep the original
path. CPU contracts verify native final state/side effects and no extra draws.
No duplicate geometry/transparency workaround or renderer change is introduced.

Opt-in EOD_WHEEL_TRACE wraps actual gameplay draw00220788 and logs the native
LOD, fast flag and selected handle. 00220740 is the simpler menu wheel draw,
not the gameplay entry. 220848 tests instance.flags&0x10;220bc8..bd8 chooses
the class LOD's +2c fast handle or +28 slow handle.

The final isolated7800-frame run exited0, headless/muted1x4:3 with invincibility,
godmode and turbo explicitly off. Human slot13 is Cupcar; all four wheel instances
reference ncu. Native class table holds only mid+ncu (2/8). Logs prove all five
ncuNf resources loaded and every human wheel selected both slow and fast handles:
front LOD1 slow661→fast662, rear LOD0 slow65f→fast660. No missing wheel resources,
wheelmod errors, panic or fallback. Preserve package and executable together;
the old installed executable lacks the independent-fast resource hook.
