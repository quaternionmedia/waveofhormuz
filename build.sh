#!/usr/bin/env bash
# build.sh — Wave of Hormuz build helper
#
# Usage:
#   ./build.sh            # build plugin
#   ./build.sh clean      # clean build artefacts
#   ./build.sh install    # build + copy .vcvplugin to Rack2 plugins folder
#   ./build.sh dist       # build + create distributable zip
#
# Set RACK_DIR if auto-detection fails:
#   RACK_DIR="$HOME/Rack-SDK" ./build.sh

set -eo pipefail

# ── Colour output ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${CYAN}[info]${NC}  $*"; }
ok()    { echo -e "${GREEN}[ok]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC}  $*"; }
err()   { echo -e "${RED}[error]${NC} $*" >&2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$SCRIPT_DIR"

# Read SLUG/VERSION from plugin.json directly (no jq needed)
SLUG="$(grep -o '"slug": *"[^"]*"' "$PLUGIN_DIR/plugin.json" | head -1 | sed 's/.*: *"\(.*\)"/\1/')"
VERSION="$(grep -o '"version": *"[^"]*"' "$PLUGIN_DIR/plugin.json" | head -1 | sed 's/.*: *"\(.*\)"/\1/')"

# ── Auto-add MinGW to PATH, strongly preferring MSYS2 MINGW64 (MSVCRT) ──────
# libRack.dll is built against MSVCRT + dynamic libstdc++.
# Compiling with a UCRT-based gcc causes heap corruption at runtime because
# strings/objects cross the DLL boundary with mismatched allocators.
# MSYS2 mingw64 (x86_64-w64-mingw32, MSVCRT) is the correct toolchain.
MINGW_CANDIDATES=(
    "/c/msys64/mingw64/bin"          # MSYS2 MINGW64  ← correct CRT (MSVCRT)
    "/c/msys64/usr/bin"
    "/c/ProgramData/mingw64/mingw64/bin"   # Chocolatey mingw (UCRT — will warn)
    "/c/mingw64/bin"
    "/c/msys64/ucrt64/bin"           # MSYS2 UCRT64   ← wrong CRT (will warn)
)
if ! command -v gcc &>/dev/null; then
    for candidate in "${MINGW_CANDIDATES[@]}"; do
        if [[ -x "$candidate/gcc.exe" ]]; then
            export PATH="$candidate:$PATH"
            break
        fi
    done
fi

# ── Check all prerequisites upfront ───────────────────────────────────────
ERRORS=0

if ! command -v make &>/dev/null; then
    err "'make' not found."
    echo "       → Chocolatey (as Admin): choco install make" >&2
    ERRORS=$((ERRORS + 1))
else
    ok "make: $(make --version | head -1)"
fi

if ! command -v gcc &>/dev/null; then
    err "'gcc' not found."
    echo "       → Install MSYS2 from https://msys2.org (or: choco install msys2)" >&2
    echo "         Then in the MSYS2 MINGW64 shell:" >&2
    echo "           pacman -S mingw-w64-x86_64-toolchain" >&2
    ERRORS=$((ERRORS + 1))
else
    ok "gcc:  $(gcc --version | head -1)  [$(gcc -dumpmachine 2>/dev/null)]"
fi

# Locate Rack SDK
SDK_CANDIDATES=(
    "${RACK_DIR:-}"
    "$HOME/Rack-SDK"
    "$HOME/Documents/Rack-SDK"
    "$HOME/Documents/Rack2/SDK"
    "/c/Users/$USERNAME/Documents/Rack-SDK"
    "$PLUGIN_DIR/../Rack-SDK"
    "$PLUGIN_DIR/../../Rack-SDK"
)

FOUND_SDK=""
for candidate in "${SDK_CANDIDATES[@]}"; do
    if [[ -n "$candidate" && -f "$candidate/plugin.mk" ]]; then
        FOUND_SDK="$candidate"
        break
    fi
done

if [[ -z "$FOUND_SDK" ]]; then
    err "Rack SDK not found  (looked for plugin.mk in):"
    for candidate in "${SDK_CANDIDATES[@]}"; do
        [[ -n "$candidate" ]] && echo "         $candidate" >&2
    done
    echo "" >&2
    echo "       → Download from: https://vcvrack.com/downloads" >&2
    echo "         Extract so that <path>/plugin.mk exists, then:" >&2
    echo "         RACK_DIR=/your/path ./build.sh" >&2
    ERRORS=$((ERRORS + 1))
else
    ok "SDK:  $FOUND_SDK"
    RACK_DIR="$FOUND_SDK"
fi

[[ $ERRORS -gt 0 ]] && { echo ""; err "$ERRORS prerequisite(s) missing — fix above then re-run."; exit 1; }

# ── Rack2 user plugin install directory ───────────────────────────────────
LOCALAPPDATA_UNIX="$(cygpath -u "${LOCALAPPDATA:-}" 2>/dev/null || echo "$HOME/AppData/Local")"
INSTALL_DIR="$LOCALAPPDATA_UNIX/Rack2/plugins-win-x64"

# Pass SLUG and VERSION on the command line so plugin.mk never needs jq
MAKE_FLAGS="RACK_DIR=$RACK_DIR SLUG=$SLUG VERSION=$VERSION"

# ── Targets ───────────────────────────────────────────────────────────────
CMD="${1:-build}"

case "$CMD" in
  build)
    info "Building $SLUG $VERSION..."
    make -C "$PLUGIN_DIR" $MAKE_FLAGS
    ok "Build complete → plugin.dll"
    ;;

  clean)
    info "Cleaning..."
    make -C "$PLUGIN_DIR" $MAKE_FLAGS clean
    ok "Clean complete."
    ;;

  install)
    info "Building $SLUG $VERSION..."
    make -C "$PLUGIN_DIR" $MAKE_FLAGS

    DIST_FILE=$(find "$PLUGIN_DIR" -maxdepth 1 -name "*.vcvplugin" | head -1)
    if [[ -z "$DIST_FILE" ]]; then
        # Fall back to plugin.dll if dist target hasn't been run
        DIST_FILE=$(find "$PLUGIN_DIR" -maxdepth 1 -name "plugin.dll" | head -1)
    fi
    [[ -n "$DIST_FILE" ]] || { err "No plugin.dll or .vcvplugin found after build."; exit 1; }

    mkdir -p "$INSTALL_DIR/$SLUG"
    cp "$DIST_FILE" "$INSTALL_DIR/$SLUG/"
    # Copy resources too if present
    [[ -d "$PLUGIN_DIR/res" ]] && cp -r "$PLUGIN_DIR/res" "$INSTALL_DIR/$SLUG/"
    [[ -f "$PLUGIN_DIR/plugin.json" ]] && cp "$PLUGIN_DIR/plugin.json" "$INSTALL_DIR/$SLUG/"

    ok "Installed: $INSTALL_DIR/$SLUG/"
    echo -e "${BOLD}Restart VCV Rack to load the plugin.${NC}"
    ;;

  dist)
    info "Building distributable for $SLUG $VERSION..."
    make -C "$PLUGIN_DIR" $MAKE_FLAGS dist
    ok "Distribution package created."
    ;;

  *)
    err "Unknown command: $CMD"
    echo "  Valid commands:  build | clean | install | dist" >&2
    exit 1
    ;;
esac
