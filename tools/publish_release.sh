#!/usr/bin/env bash
# Build a public macOS prerelease from a committed revision on origin/main.
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
ctest --test-dir "$build" -j4 -E AssetTools --output-on-failure
app="$stage/Eve of Destruction.app"
env -u EOD_BUNDLE_MODS "$python" tools/macos/bundle.py "$app" "$build/eod"
# Public packages launch with the player's own disc and no personal mod data.
/usr/libexec/PlistBuddy -c 'Add :LSEnvironment:EOD_NATIVE_ONLY string 1' "$app/Contents/Info.plist"
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
[[ ! -e "$app/Contents/Resources/Mods" && ! -e "$app/Contents/Resources/DATA" ]]
ditto -c -k --keepParent --norsrc "$app" "$stage/Eve-of-Destruction-macOS-arm64.zip"
gh release create "$tag" "$stage/Eve-of-Destruction-macOS-arm64.zip" --repo "$repo" --target "$revision" --prerelease --title "Online play preview ($tag)" --notes "Experimental online lobby, car and track selection, voice and loading changes. Requires your own North American game disc. macOS download is for Apple Silicon, locally signed, without personal mods. Internet and hardware voice testing are ongoing."
