# Vehicle OBJ export and reimport

Tools: `tools/vehicle_export.py`, `tools/vehicle_import.py` (on `tools/vif_patch.py`, `tools/mod_package.py`).
Run unit tests from the repository root: `EOD_HEADLESS=1 EOD_MUTE=1 EOD_TEST_RETAIL_MODELS=1 python3 -m unittest tools.tests.test_vehicle_export tools.tests.test_vehicle_import`.
Never run these from inside `tools/` (`tools/dis.py` shadows the standard library).

## Player success

A modder can dump any retail car fully textured, edit it in Blender, and get the edit into a
Kit package that the native game loads as the same car. Limits below are the honest scope.
Native playtest of an import is not established by this document.

## Export

```sh
python3 tools/vehicle_export.py --disc "$DISC" --car DATA/CAR/BUS.CAR --dest out [--lod 1|1,2|all|shadow] [--paint 790] [--blender] [--preview p.png]
python3 tools/vehicle_export.py --disc "$DISC" --all --dest out --lod all
```

Per car `out/<STEM>/`: `<STEM>.obj`, `.mtl`, `textures/*.png`, `vehicle.json` (`--blender` adds `import_blender.py`).
- Objects are `<model>__slotNN_g<damageGroup>_<texture>[_body]` (model = `Low1`, `shadow1`, `Wheel_mid1`).
  Each is preceded by a comment with assetId (`DATA/CAR/BUS.CAR#Low1.mod`), slot, damage group, body-panel bit, raw group.
  The low 16 group bits are the native damage/visibility group; bit 16 marks paintable body panels. Overlapping groups are
  native variants and all render together in an OBJ viewer.
- Vertices of every object are in native VIF order with per-vertex `vt`/`vn` (`vt v = 1 - native t`); degenerate triangles are dropped.
- Materials resolve with `modding_backend.resolve_material` and the Kit's default livery (`vehicle_paint`, first retail driver of the
  chassis, e.g. Bus 790; Bus2 uses CT.RES `79a.tm0`). `--paint N` selects CT.RES livery N and fails if absent. Unresolved
  materials are in `vehicle.json.unresolvedMaterials` and the MTL (magenta `Kd`, no `map_Kd`); none are substituted.
  Textures are the member the resolver returns (embedded low-resolution `.tex` before `.tm0`), mip zero.
- Wheels: RACE.RES wheel-class meshes at the origin. Left/right front/rear markers are damage anchors that do not match
  wheel centres or physics tracks, so no native socket position is verified. Markers and physics.val/lod.val/bone.val-derived
  data are in `vehicle.json` (`markers`, `physics`, `lod`), with sha256 for every member, the archive and SLUS.
- `--all` writes `export-summary.json`; per-car exceptions are recorded in `failures`, never hidden.
  Retail disc result (`--lod all`): 37 cars, 395 models, 3,244 parts, 533,863 vertices, 288,917 triangles, 431 materials,
  12 unresolved (RollBar.tex on Gladiator/Hurricane; Hobby.tex and HWindows.tex on five trailers), 0 failures.

## Import

`vehicle_import.py patch --disc D --car DATA/CAR/X.CAR --obj edited.obj [--texture MEMBER=PNG ...] --out PKG --id ID`
Same-topology edit of an existing retail car -> verified Kit package (`eodmod.json`, `native/<archive>/<member>`, contract section 5).
- Geometry: positions, UVs, normals only, via `vif_patch` (same member length, same quantization; topology, vertex count,
  face connectivity, new/removed parts and objects are refused with the model named). Objects of one OBJ are split by model prefix;
  untouched models are left byte-identical. `Wheel_*` objects are ignored (wheels belong to RACE.RES, not the car).
  A no-edit roundtrip is byte-identical for all 278 retail car models (test).
- Textures: `--texture Bus1.tex=p.png` or `DATA/RES/CT.RES#790.tm0=p.png`; PNG must equal the native size; recompiled with the
  original header parameters (256-colour quantization, lossy); layout changes are refused.
- Nothing written when no member changed.

`vehicle_import.py new --disc D --base Algar --name Zed --number 777 --class midsize [--body-png 512x256.png] [--obj edited.obj]`
New named chassis: `create_custom_car` overlay (`DATA/CAR/ZED.CAR`, COMMON/CT/UICAR.RES) for `EOD_MOD_DIR`; optional body PNG replaces
`Name.tm0` only (512x256), optional same-topology OBJ edit is applied to the clone's models. Limits are create_custom_car's:
base must be a verified retail chassis (31 in `vehicle_materials.BODY_TEXTURES`; trailers refused), name <= 8 letters/digits, exactly one class,
free racing number, geometry topology = base. This output is an overlay directory, not an `eodmod.json` package: a new CAR file and
new COMMON.RES rows are not replacements of retail members, which section 5 cannot express.

## Not supported

Arbitrary new meshes (a different vertex/face count or a mesh from another game), new wheel/marker layouts, new damage groups,
skinning/bone edits, shadow-volume authoring beyond same-topology moves, `.tm0` high-resolution paint of CT.RES liveries
beyond same-size replacement, and combining a new-chassis overlay with other COMMON.RES overlays.

## Open questions

Native wheel socket positions; whether per-vertex skin weights should follow edited positions; Name.tex low-res generation for
new-chassis PNGs; Blender script is untested.
