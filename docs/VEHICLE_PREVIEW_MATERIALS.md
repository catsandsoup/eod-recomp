# Vehicle preview material binding — 10 November 2026

Current follow-up: see `BUS_STICKER_WORKFLOW.md`. Native Bus race evidence showed its embedded templates are replaced by CT.RES paint resources; model inspection now applies saved/pending edits and native paint context. The literal-precedence statement below describes the earlier resolver checkpoint.

## Player success

The Ambulance Low1 model in the development Kit displays its native painted
body instead of reporting a missing amby.tex. Embedded vehicle textures retain
precedence; another chassis's paint is never chosen merely because a filename
matches. This does not establish full vehicle assembly, wheels or game lighting.

## Situation

The screenshot identifies AMBULA_1.CAR/Low1.mod by size 126,052 and SHA256 prefix
4394a450d6e4bf0e. Its body material amby.tex is an alias, not a literal retail
texture member. Literal-only preview lookup omitted native numbered liveries.

## Task and action

Reuse the existing verified BODY_TEXTURES map from create_custom_car, moving it
into vehicle_materials.py. Match the native 8.3 chassis filename, then a real
COMMON.RES/dirt.val Driver chassis and number, then CT.RES/<number>.tm0 or .tex.
Prefer embedded/resident literal textures; never borrow another car's alias.
Show the default preview livery explicitly. This is a deterministic first matching
driver, not a user-selectable paint or a claim to reproduce every game selection.
Also search PART.RES: actual retail race logs load this resource set, and it
contains Hearse's Brake.tex. Keep unresolved names visible.

## Result

Observed the actual Kit showing the textured Ambulance with livery 601, William
Anderson. All 28 recovered parts have decoded textures. Seven Kit CTest suites
passed, including retail model/texture coverage. The isolated development game
ran Ambulance in Anoka for 300 frames, produced first-race.png and exited 0.
These are macOS results; Windows deployment was not attempted. Fifteen protected
working-app/game/project hashes remained unchanged.

Known unresolved names remain RollBar.tex (Gladiator/Hurricane), Hobby.tex and
HWindows.tex (five trailers). No arbitrary texture substitutes were installed.
The next decisive test is to recover their native material remapping and compare
those specific models to the game's output. Complete wheel/driver assembly and
lighting remain separate work.

## Reuse references

Checked https://github.com/silentsudin/RoadTripAdventure-recomp on 10 November
2026. Its renderer texture-pack mechanism is a different layer; this fix binds
EOD's own named archive materials and numbered driver paints. Existing local
create_custom_car.py and native retail tables supply the applicable contract;
no external rendering subsystem was added.
