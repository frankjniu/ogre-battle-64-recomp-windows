#!/usr/bin/env bash
# Capture the black sprite part in one key press (session 131).
#
# The developer reaches the fault and holds the key down while it is on screen.
# One press writes both halves of the evidence into <outdir>:
#
#   ram-1-full.bin   the whole 8 MiB RDRAM image at that instant
#                    (read it with tools/ogre_ram.py)
#   tex/              one file per textured rectangle the frame draws, plus the
#                    [tex] lines in run.log
#                    (read them with tools/ogre_tex_uv.py)
#
# There is no timer: the key is what takes the dump, and OGRE_TEX_TRACE_MS keeps
# the trace open long enough that a press during the shot captures it.
#
# Usage:
#   debug/black-sprite-capture.sh [app-binary] [save] [outdir]
#
# Defaults:
#   app    build-dist/ogrebattle64   (set by `make app` in this tree)
#   save   ~/Desktop/ogre-battle-64-recomp/saves/scene_14.bin
#   outdir ~/Desktop/ogre-battle-64-recomp/black-sprite
#
# With no save argument the run is left at the title; the route is then yours.
# Nothing here writes to the tree: the preference directory is <outdir>/pref, so
# the battery, the settings and every capture land under <outdir>.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${1:-$ROOT/build-dist/ogrebattle64}"
SAVE="${2:-$HOME/Desktop/ogre-battle-64-recomp/saves/scene_14.bin}"
OUT="${3:-$HOME/Desktop/ogre-battle-64-recomp/black-sprite}"

# Resolve the paths now: the run changes to $OUT so the game's files land beside
# the captures, and a relative app or save path would stop resolving there.
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
if [[ -n "$SAVE" && ! -f "$SAVE" ]]; then
    echo "no save at $SAVE" >&2
    exit 2
fi

mkdir -p "$OUT/pref"
if [[ -n "$SAVE" ]]; then
    # OGRE_SAVE loads a battery image into the preference directory before boot.
    # The game writes the battery back and the port then keeps the newer copy, so
    # a re-run would start from the previous run's progress. Resetting makes every
    # run start from the same file; OGRE_SAVE_RESET=0 on this command line keeps
    # the progress a run wrote back instead.
    export OGRE_SAVE="$SAVE"
    export OGRE_SAVE_RESET="${OGRE_SAVE_RESET:-1}"
fi

export OGRE_PREF_DIR="$OUT/pref"
# AGENTS.md rule 14: every run of the game skips the audio output device.
export OGRE_NO_AUDIO=1
export OGRE_RAM_KEY=1 OGRE_RAM_KEY_FULL=1 OGRE_RAM_KEY_OUT="$OUT"
# Key 2 arms the trace and key 3 arms it too, with its own edge state: the two
# keys are independent so one run can bracket a pose change.
#
# No per-draw dump by default. The colour report is what names a black draw, and
# it is in run.log; the dump writes two files per rectangle, which a capture that
# runs to the window close turns into tens of thousands of files. Set
# OGRE_TEX_TRACE_DUMP to a directory on the command line when the files are
# actually wanted (tools/ogre_tex_uv.py reads them).
export OGRE_TEX_TRACE=1 OGRE_TEX_TRACE_KEY=2 OGRE_TEX_TRACE_KEY2=3 \
       OGRE_TEX_TRACE_MS="${OGRE_TEX_TRACE_MS:-0}" \
       OGRE_TEX_TRACE_COLOUR=1 OGRE_TEX_TRACE_BLACK="${OGRE_TEX_TRACE_BLACK:-8}"
if [[ -n "${OGRE_TEX_TRACE_DUMP:-}" ]]; then
    export OGRE_TEX_TRACE_DUMP
fi
export OGRE_SCENE_LOG=1

echo "app     $APP"
echo "save    ${SAVE:-<none>}"
echo "outdir  $OUT"
echo
echo "Wire it to the fault like this:"
echo "  1   just BEFORE the part turns black   -> $OUT/ram-1-full.bin (the good state)"
echo "  2   AT the moment it is black          -> the trace starts; the bad frames are in"
echo "                                            run.log from here to the window close"
echo "  1   again AFTER it happened            -> $OUT/ram-2-full.bin (the bad state)"
echo
echo "The two full images are the same buffers before and after, so one run compares"
echo "them. 3 arms the trace with its own edge state, for a second attempt in the same"
echo "run; the same key pressed again re-arms it too."
echo
echo "The trace prints a colour mean per textured rectangle and one"
echo "  [tex] BLACK DRAW: ..."
echo "line for the first one that paints black. That line is the result; find it"
echo "with:  grep -n 'BLACK DRAW' $OUT/run.log"
echo
echo "Then close the window. If the game runs fast, set OGRE_SPEED=1 on this"
echo "command line to play at normal speed."
echo

(cd "$OUT" && "$APP" 2>&1 | tee "$OUT/run.log")
echo
echo "wrote $OUT/run.log"
ls -la "$OUT"/ram-*-full.bin 2>/dev/null || echo "no full-RDRAM dump: key 1 was not pressed"
ls "$OUT/texdump/tex" 2>/dev/null | wc -l | sed 's/^/texture dump files: /'
