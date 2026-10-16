#!/bin/sh
# Verify a relocated personal app uses its bundled Vulkan loader/ICD and GPU.
# Every invocation is headless and muted. No disc data is copied or published.
set -eu
app=${1:?usage: test_app_vulkan.sh APP GS_CAPTURE [LOG]}
fixture=${2:?usage: test_app_vulkan.sh APP GS_CAPTURE [LOG]}
scratch=$(mktemp -d /private/tmp/eod-app-vulkan.XXXXXX)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
copy="$scratch/Relocated Game.app"
log=${3:-"$scratch/replay.log"}
mkdir -p "$scratch/home" "$scratch/tmp" "$scratch/empty"
ditto --noextattr "$app" "$copy"
codesign --verify --deep --strict "$copy"
# The intentionally broken external ICD overrides must be replaced by the app's
# own startup discovery. No user's loader/layer/XDG environment is inherited.
if ! env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="$scratch/tmp/" \
    EOD_HEADLESS=1 EOD_MUTE=1 EOD_APP_SUPPORT="$scratch/support" \
    XDG_CONFIG_HOME="$scratch/empty" XDG_DATA_HOME="$scratch/empty" \
    VK_DRIVER_FILES="$scratch/nonexistent.json" VK_ICD_FILENAMES="$scratch/nonexistent.json" \
    VK_LAYER_PATH="$scratch/empty" VK_LOADER_DEBUG=driver \
    "$copy/Contents/MacOS/eod" --headless --renderer vulkan --gs-replay "$fixture" --repeat 3 > "$log" 2>&1; then
    cat "$log" >&2; exit 1
fi
# A soft fallback is a test failure, even if the process exits successfully.
/usr/bin/grep -q 'replaying with vulkan' "$log"
/usr/bin/grep -Eq '\[vulkan\] .+ API [0-9]' "$log"
/usr/bin/grep -Fq "$copy/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json" "$log"
/usr/bin/grep -q 'libMoltenVK.dylib' "$log"
/usr/bin/grep '\[vulkan\]' "$log"
echo 'Relocated signed app replay passed with its bundled ICD and GPU.'
