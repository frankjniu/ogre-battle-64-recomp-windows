#!/usr/bin/env bash
# One run that names the texture the map unit's sprites are bound to.
#
# The black head's colour tile has correct RDRAM, correct tile descriptors and a
# correct TMEM snapshot (`docs/HANDOFF-2026-10-11-session133.md` §14), so the
# remaining question is which texture the draw resolves to and what the renderer
# decodes out of the TMEM it hashes. This script turns on three read-only
# diagnostics that answer it from run.log, plus the whole-image key press so the
# same frame can be read offline:
#
#   OGRE_SPRITE_TRACE=1     one line per small tile:
#                             [spr] call=<draw> tile=<i>/<n> cycle=.. combine=..
#                                   prim=.. env=.. fmt/siz/line/tmem sample=WxH
#                                   tileCopy=.. raw=.. hash=.. rdp(addr=.. stride=..
#                                   masks=.. native=..)
#   OGRE_TILE_DECODE=1      one line per hashed tile: the RGBA the renderer would
#                           decode out of the TMEM snapshot it is hashing
#                             [dec] hash=.. fmt=.. siz=.. line=.. tmem=.. WxH
#                                   cached=.. nz=../.. mean=.. px00=.. pxmid=.. pxlast=..
#   OGRE_TILE_TRACE_ALL=1   the texture the cache handed back
#     OGRE_TILE_TRACE_MAXH=64   [gpu-tile] .. idx=.. dims=.. found=..
#                           (the 64 cap keeps the 320-wide njpeg strips out)
#
# The RT64-side fix for the black head refuses a framebuffer tile copy whose
# framebuffer is out of sync with RDRAM (`[fbcopy-stale]`). The saved whole-image
# dump at the fault moment renders the frame, so the head can be checked without
# a screenshot: `python3 tools/ogre_frame.py ram-1-full.bin --out f/`.
#
# This second version adds:
#   [fbcopy]   a tile whose TMEM range is covered by a region that has a
#              framebuffer tile copy (that path skips the texture cache)
#   [dec] rawTMEM=  the raw-TMEM upload path, which also skips the texture cache
# The sprite trace now prints only two-texture draws, so it survives a whole run.
#
#   debug/head-trace.sh [app] [save] [outdir]
#
# Drive to a screen where a unit's head is black, press `1` once (that writes
# ram-1-full.bin for the same frame the log covers), then close the window.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${1:-$ROOT/build-app/ogrebattle64}"
SAVE="${2:-$HOME/Desktop/ogre-battle-64-recomp/saves/scene_14.bin}"
OUT="${3:-$HOME/Desktop/ogre-battle-64-recomp/head-trace}"
ROM="${OGRE_ROM:-$HOME/Desktop/ogre-battle-64-recomp/ogrebattle64-us-rev1.z64}"

export OGRE_ROM="$ROM"
export OGRE_SPRITE_TRACE=1
export OGRE_TILE_DECODE=1
export OGRE_TILE_TRACE_ALL=1
export OGRE_TILE_TRACE_MAXH=64
export OGRE_TILE_TRACE_MAX=5000
# The texture trace is not needed for this run; leave it disarmed.
unset OGRE_TEX_TRACE OGRE_TEX_TRACE_KEY OGRE_TEX_TRACE_KEY2 OGRE_TEX_TRACE_IMAGES || true

exec "$ROOT/debug/frame-capture.sh" "$APP" "$SAVE" "$OUT"
