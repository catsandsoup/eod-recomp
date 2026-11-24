#!/usr/bin/env bash
# Build a public macOS release from a committed revision on origin/main.
set -euo pipefail
repo=catsandsoup/eod-recomp
root=$(git rev-parse --show-toplevel)
cd "$root"
tag=${1:?usage: publish_release.sh TAG}
build=${EOD_BUILD_DIR:?set EOD_BUILD_DIR to the configured native build directory}
python=${EOD_PYTHON:-python3}
revision=$(git rev-parse HEAD)
[[ -z $(git status --porcelain --untracked-files=normal) ]]
[[ $(git ls-remote "https://github.com/$repo.git" refs/heads/main | cut -f1) == "$revision" ]]
stage=$(mktemp -d /private/tmp/eod-release.XXXXXXXX)
trap 'rm -rf "$stage"' EXIT
export EOD_HEADLESS=1 EOD_MUTE=1
"$python" tools/recomp.py
cmake --build "$build"
ctest --test-dir "$build" -j4 --output-on-failure
app="$stage/Eve of Destruction.app"
env -u EOD_BUNDLE_MODS "$python" tools/macos/bundle.py "$app" "$build/eod"
# Public packages launch with the player's own disc and no personal mod data.
/usr/libexec/PlistBuddy -c 'Add :LSEnvironment:EOD_NATIVE_ONLY string 1' "$app/Contents/Info.plist"
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
[[ ! -e "$app/Contents/Resources/Mods" && ! -e "$app/Contents/Resources/DATA" ]]
ditto -c -k --keepParent --norsrc "$app" "$stage/Eve-of-Destruction-macOS-arm64.zip"
"$python" tools/package_macos_world_editor.py --game-executable "$build/eod" \
  --plume-library "$build/eod_editor_plume.dylib" --output "$stage/world-editor"
"$python" tools/package_creation_kit_release.py --game-executable "$build/eod" --output "$stage/creation-kit"
mv "$stage/world-editor/EOD-World-Editor-macOS-arm64.zip" "$stage/"
mv "$stage/creation-kit/EOD-Creation-Kit-Development-macOS-arm64.zip" "$stage/"
assets=("$stage/Eve-of-Destruction-macOS-arm64.zip"
        "$stage/EOD-World-Editor-macOS-arm64.zip"
        "$stage/EOD-Creation-Kit-Development-macOS-arm64.zip")
# Supply the matching, tested Windows package to publish both platforms together.
if [[ -n ${EOD_WINDOWS_ARCHIVE:-} ]]; then
  cp "$EOD_WINDOWS_ARCHIVE" "$stage/Eve-of-Destruction-Windows-x64.zip"
  assets+=("$stage/Eve-of-Destruction-Windows-x64.zip")
fi
(cd "$stage" && shasum -a 256 ./*.zip > SHA256SUMS)
assets+=("$stage/SHA256SUMS")
notes="$root/dist/release-v1.0.md"
title="Eve of Destruction Remaster ${tag#v}"
release_flags=(--prerelease=false --latest)
case "$tag" in
  v1.1.1|v1.1.1-*)
    title="Eve of Destruction Remaster 1.1.1 prerelease"
    notes="$root/dist/prerelease-1.1.1.md"
    release_flags=(--prerelease --latest=false)
    ;;
esac
gh release create "$tag" "${assets[@]}" --repo "$repo" --target "$revision" --draft --title "$title" --notes-file "$notes" "${release_flags[0]}"
gh release edit "$tag" --repo "$repo" --draft=false "${release_flags[@]}"
