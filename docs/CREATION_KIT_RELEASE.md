# EOD Creation Kit — development preview

A separate macOS Apple Silicon editing app from the same eod-recomp repository.
The existing Eve of Destruction and EOD Modding Suite apps are not replaced.
This is an unfinished development Kit, not a complete Creation Kit release.

## Install and use

Extract the ZIP and move EOD Creation Kit Development.app to Applications.
Use your own extracted USA retail disc folder (DATA/ and SLUS_209.10); select it
with Game Folder. macOS Command Line Tools must supply /usr/bin/python3.
An isolated development runtime and its shared libraries are included. No retail
assets, personal projects, memory cards, or prebuilt retail mod data are shipped.
The app is ad-hoc signed, not notarized; macOS may require approval in Privacy &
Security. This packaged runtime currently requires macOS 27 because of its linked
libraries; the editor source targets macOS 14. This download is arm64 only; no Windows Kit
binary has been built or tested.

Projects default to ~/Library/Application Support/EODCreationKit/mods/.
Open Project can choose a different project location. Choose a real track to
preview its recovered world and supported assets. Edit native table values or
export/import supported indexed PNG textures at their original dimensions,
Save & Build, then Run Mod in Game. Saving creates a separate native overlay;
it does not overwrite the source disc. Test sessions use a separate memory card.
Use Game → Choose Game Executable only if overriding the bundled test runtime.

## Verified and remaining scope

Native archive browsing, recovered model/track previews, numbered vehicle body
livery binding, native table editing, supported texture PNG export/import and
native overlay save/reopen are implemented. A saved Anoka sky palette edit was
observed in an actual native race. The Ambulance material fix was observed in
the real Kit and an isolated Ambulance race ran and exited cleanly.

Known missing material remaps: RollBar.tex on Gladiator/Hurricane, and Hobby.tex
and HWindows.tex on trailers. Model preview assembly and lighting are incomplete.
Not all native formats are editable. 4K/PBR/HDR texture replacement, arbitrary
model conversion, full scene manipulation and live hot reload are not provided.
Direct race boot still performs required loading internally; suppression of
visible startup/loading frames needs wider visible-window acceptance testing.

## Build the same package

From a matching checkout, with an existing development runtime:

    EOD_HEADLESS=1 EOD_MUTE=1 python3 tools/package_creation_kit_release.py \
      --game-executable build/creation-kit-runtime/eod --output release/creation-kit-preview

The output directory must not already exist. This reuses tools/macos/bundle.py
for native dependencies and license notices. Test the packaged runtime against
your retail installation before publishing. Preserve the ZIP checksum alongside
the download. Source and license terms are available in the same repository.
