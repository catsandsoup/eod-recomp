#!/bin/sh
# Personal local packaging only; this script never uploads anything.
set -eu
app=${1:-"$HOME/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app"}
out=${2:-"$(dirname "$app")/Eve of Destruction.dmg"}
[ -d "$app" ] || { echo "Build with -DEOD_MAC_APP=ON first" >&2; exit 1; }
codesign --verify --deep --strict "$app"
stage=$(mktemp -d "${TMPDIR:-/tmp}/eod-dmg.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
ditto "$app" "$stage/Eve of Destruction.app"
ln -s /Applications "$stage/Applications"
hdiutil create -volname 'Eve of Destruction' -srcfolder "$stage" -format UDZO -ov "$out"
