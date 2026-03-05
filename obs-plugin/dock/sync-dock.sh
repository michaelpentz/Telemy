#!/usr/bin/env bash
# sync-dock.sh — sync dock source files between git repo and root-level runtime copies
# Git repo (source of truth): telemy-v0.0.4/obs-plugin/dock/
# Root-level (OBS runtime):   E:/Code/telemyapp/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

DOCK_FILES=(
  aegis-dock.jsx
  aegis-dock-entry.jsx
  aegis-dock-bridge.js
  aegis-dock-bridge-host.js
  aegis-dock-browser-host-bootstrap.js
  aegis-dock.html
)

cmd_build() {
  echo "Building aegis-dock-app.js from root sources..."
  cd "$ROOT_DIR"
  NODE_PATH=dock-preview/node_modules npx esbuild aegis-dock-entry.jsx \
    --bundle --format=iife --jsx=automatic \
    --outfile=aegis-dock-app.js --target=es2020 --minify
  echo "Done. Built $ROOT_DIR/aegis-dock-app.js"
}

cmd_pull() {
  echo "Pulling root-level sources into repo dock/..."
  for f in "${DOCK_FILES[@]}"; do
    if [ -f "$ROOT_DIR/$f" ]; then
      cp "$ROOT_DIR/$f" "$SCRIPT_DIR/$f"
      echo "  $f"
    else
      echo "  $f (skipped, not found at root)"
    fi
  done
  echo "Done."
}

cmd_push() {
  echo "Pushing repo dock/ sources out to root level..."
  for f in "${DOCK_FILES[@]}"; do
    if [ -f "$SCRIPT_DIR/$f" ]; then
      cp "$SCRIPT_DIR/$f" "$ROOT_DIR/$f"
      echo "  $f"
    else
      echo "  $f (skipped, not found in repo)"
    fi
  done
  echo "Done."
}

case "${1:-}" in
  build)      cmd_build ;;
  pull)       cmd_pull ;;
  push)       cmd_push ;;
  pull-build) cmd_pull; cmd_build ;;
  *)
    echo "Usage: sync-dock.sh {build|pull|push|pull-build}"
    echo ""
    echo "  build       Build aegis-dock-app.js from root sources"
    echo "  pull        Copy root-level sources INTO repo dock/"
    echo "  push        Copy repo dock/ sources OUT to root level"
    echo "  pull-build  Pull then build (most common workflow)"
    exit 1
    ;;
esac
