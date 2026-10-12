#!/usr/bin/env bash
# One key press = one whole-RDRAM image, which `tools/ogre_frame.py` renders.
#
# The black-part investigation needs the frame the fault is in, and a key press
# at that moment takes the whole 8 MiB guest image. One press is enough: the
# image holds the display lists and every buffer they sample, so the frame can
# be rendered and the faulty draw named offline, with no screenshot and no
# dependence on the in-app texture trace's own colour report.
#
#   debug/frame-capture.sh [app] [save] [outdir]
#
# Defaults:
#   app    build-dist/ogrebattle64
#   save   ~/Desktop/ogre-battle-64-recomp/saves/scene_14.bin
#   outdir ~/Desktop/ogre-battle-64-recomp/frame-capture
#
# Press `1` while the part is black. Every press writes another file
# (`ram-1-full.bin`, `ram-2-full.bin`, ...), so pressing a few times while the
# fault is on screen costs nothing and covers a bad moment.
#
# Afterwards:
#   python3 tools/ogre_frame.py <outdir>/ram-1-full.bin \
#       --log <outdir>/run.log --out <outdir>/frames/
#
# Nothing is written into the tree: OGRE_PREF_DIR is <outdir>/pref, so the
# battery, the settings and the dumps all land under <outdir>.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${1:-$ROOT/build-dist/ogrebattle64}"
SAVE="${2:-$HOME/Desktop/ogre-battle-64-recomp/saves/scene_14.bin}"
OUT="${3:-$HOME/Desktop/ogre-battle-64-recomp/frame-capture}"

abs_path() {
    local p="$1" dir
    [[ -z "$p" ]] && return 0
    dir="$(cd "$(dirname "$p")" 2>/dev/null && pwd)" || return 0
    printf '%s/%s\n' "$dir" "$(basename "$p")"
}
APP="$(abs_path "$APP")"
SAVE="$(abs_path "$SAVE")"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

if [[ ! -x "$APP" ]]; then
    echo "no app at $APP -- run 'make app' first" >&2
    exit 2
fi

mkdir -p "$OUT/pref"
# The dump path: OGRE_RAM_KEY=<digit> plus OGRE_RAM_KEY_FULL=1 writes the whole
# image on every press, numbered so nothing is overwritten.
export OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1 OGRE_RAM_KEY_OUT="$OUT"
export OGRE_PREF_DIR="$OUT/pref"
# AGENTS.md rule 14: every run of the game skips the audio output device.
export OGRE_NO_AUDIO=1
# One [scene] line per scene change, so the log says which screen the press was
# on.
export OGRE_SCENE_LOG=1
# Key 2 arms the texture trace; key 1 is the dump.
#
# OGRE_TEX_TRACE_IMAGES=1 fingerprints every image the display list binds **at
# submission**, which is the datum that separates "the renderer read the buffer
# before the game wrote it" from "the game filled it before the draw": the dump is
# taken on the main thread, the fingerprint when the list is submitted. It costs
# nothing unless key 2 is pressed, because the trace is edge-armed.
export OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_KEY=2 OGRE_TEX_TRACE_KEY2=3 \
       OGRE_TEX_TRACE_IMAGES=1 OGRE_TEX_TRACE_MS=0
if [[ -n "${OGRE_TEX_TRACE_DUMP:-}" ]]; then
    export OGRE_TEX_TRACE_DUMP
fi
if [[ -n "$SAVE" && -f "$SAVE" ]]; then
    export OGRE_SAVE="$SAVE"
    export OGRE_SAVE_RESET="${OGRE_SAVE_RESET:-1}"
fi

echo "app     $APP"
echo "save    ${SAVE:-<none>}"
echo "outdir  $OUT"
echo
echo "Press 1 in the game window while the part is black. Every press writes"
echo "  $OUT/ram-<n>-full.bin"
echo "Press 2 as well, at the same moment: it arms the texture trace, which"
echo "  fingerprints every image the display list binds (see the header)."
echo "Then close the window. Add OGRE_SPEED=1 on this command line for normal speed."
echo
echo "To list the tiles that sample RT64's fallback texture (session 133), add"
echo "  OGRE_TILE_TRACE_ALL=1 OGRE_TILE_TRACE_MISS=1 OGRE_TILE_TRACE_MAX=2000"
echo "to this command line and grep '[gpu-tile]' in run.log; an empty list means"
echo "every tile was found in the texture cache."
echo

(cd "$OUT" && "$APP" 2>&1 | tee "$OUT/run.log")
echo
ls -la "$OUT"/ram-*-full.bin 2>/dev/null || echo "no dump: key 1 was not pressed"
