#!/usr/bin/env bash
# One-click macOS launcher for EOD World Editor (with native Plume HQ + PIE support).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENV_PY="$SCRIPT_DIR/.venv/bin/python"

if [[ ! -x "$VENV_PY" ]]; then
  VENV_PY="$(command -v python3)"
fi

# Guard against macOS UF_HIDDEN flags on .venv PySide6 Qt plugins (prevents 'cocoa not found')
QT_PLUGINS="$SCRIPT_DIR/.venv/lib/python3.13/site-packages/PySide6/Qt/plugins"
if [[ -d "$QT_PLUGINS" ]]; then
  chflags -R nohidden "$QT_PLUGINS" 2>/dev/null || true
fi

# Wire native macOS Plume build & Homebrew MoltenVK/Vulkan loader when present
if [[ -f "$REPO_ROOT/build-mac-plume/eod_editor_plume.dylib" ]]; then
  export EOD_EDITOR_PLUME_LIBRARY="${EOD_EDITOR_PLUME_LIBRARY:-$REPO_ROOT/build-mac-plume/eod_editor_plume.dylib}"
fi
if [[ -f "/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json" ]]; then
  export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json}"
  export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$VK_DRIVER_FILES}"
fi
if [[ -d "/opt/homebrew/opt/vulkan-loader/lib" ]]; then
  export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/opt/vulkan-loader/lib:/opt/homebrew/opt/molten-vk/lib:/opt/homebrew/opt/sdl3/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}"
fi

export PYTHONPATH="$REPO_ROOT/tools:$SCRIPT_DIR:${PYTHONPATH:-}"
cd "$REPO_ROOT"
exec "$VENV_PY" "$SCRIPT_DIR/app.py" "$@"
