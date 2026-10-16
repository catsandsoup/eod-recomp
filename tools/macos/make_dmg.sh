#!/bin/sh
# tools/macos/make_dmg.sh "<build>/Eve of Destruction.app" [out.dmg]: a disk image
# with the app and a link to /Applications (build the app with -DEOD_MAC_APP=ON).
set -eu
app=${1:?usage: tools/macos/make_dmg.sh "<build>/Eve of Destruction.app" [out.dmg]}
out=${2:-"$(dirname "$app")/Eve of Destruction.dmg"}
[ -d "$app" ] || { echo "no $app: build with -DEOD_MAC_APP=ON first" >&2; exit 1; }
codesign --verify --deep --strict "$app"
stage=$(mktemp -d "${TMPDIR:-/tmp}/eod-dmg.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
ditto "$app" "$stage/Eve of Destruction.app"
ln -s /Applications "$stage/Applications"
hdiutil create -volname 'Eve of Destruction' -srcfolder "$stage" -format UDZO -ov "$out"
