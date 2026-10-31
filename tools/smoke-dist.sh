#!/usr/bin/env bash
#
# Smoke-test a built package. Run on the machine that built it; `make smoke`
# does, and the release workflow runs it on every runner image so a package that
# cannot start fails the release instead of reaching a player.
#
#   tools/smoke-dist.sh [dist-dir]        # default: dist/ogre-battle-64-recomp
#
# Five checks:
#
#   1. the app binary, its companion files and the packaged example mods exist;
#      the mods are part of the package (`packaging/README-dist.txt` lists them)
#      and an empty `mods/` directory passed every other check, which is how
#      v0.4.0 shipped without them (session 108);
#   2. the package is self-contained. Windows: every DLL the shipped binaries
#      import is either shipped beside them or a Windows system DLL
#      (tools/pe_imports.py). This is the check that finds a missing runtime DLL
#      even on a machine that has the redistributable installed, where launching
#      the program would succeed anyway (session 94: dxcompiler.dll imports
#      MSVCP140.dll/VCRUNTIME140.dll and neither was in the package). Linux: the
#      statically linked SDL2 must carry an ALSA, PulseAudio or PipeWire backend,
#      because the OSS-only build that is otherwise possible cannot open a device
#      on any PipeWire or PulseAudio machine (session 100, issue #8);
#   3. the binary loads and reaches main: it is run with OGRE_SMOKE=1, which
#      exits 0 as soon as main is reached, under a deadline so a loader error
#      dialog or a hang cannot stall a release run. The same run asserts that the
#      presented-frame capture is off, which is the v0.4.0 Windows crash
#      (session 107);
#   4. a run with OGRE_CRASH_TEST=segv writes an error.log that holds the
#      [boot] line printed before the raise;
#   5. Windows: the same report from a launch with no standard handles (a
#      double-click), made through Explorer and a temporary shortcut.
#
# `SMOKE_TIMEOUT` (seconds, default 90) bounds checks 3 and 5.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
dist=${1:-"$root/dist/ogre-battle-64-recomp"}
timeout_seconds=${SMOKE_TIMEOUT:-90}

[ -d "$dist" ] || { echo "smoke-dist: no such package directory: $dist" >&2; exit 1; }

# --- what kind of package is this? ------------------------------------------
kind=linux
if [ -f "$dist/ogrebattle64.exe" ]; then
    kind=windows
elif [ -d "$dist/Ogre Battle 64.app" ]; then
    kind=macos
fi
echo "smoke-dist: $dist (kind=$kind)"

fail() { echo "smoke-dist: FAIL: $*" >&2; exit 1; }

case "$kind" in
    windows) app="$dist/ogrebattle64.exe" ;;
    macos)   app="$dist/Ogre Battle 64.app/Contents/MacOS/ogrebattle64" ;;
    linux)   app="$dist/ogrebattle64" ;;
esac
[ -f "$app" ] || fail "the app binary is missing ($app)"
[ -f "$dist/README.txt" ] || fail "README.txt is missing from the package"

# The package must carry the example mods the README lists. `make dist` requires
# them, and this is the check that fails on a package built before that rule:
# v0.4.0 shipped an empty mods/ directory.
mods=$( (ls "$dist"/mods/*.nrm 2>/dev/null || true) | wc -l | tr -d ' ')
[ "$mods" -gt 0 ] || fail "the package carries no mods/*.nrm (the README lists the example mods)"
echo "smoke-dist: the package carries $mods example mod(s)"

# --- 2. is the package self-contained? --------------------------------------
if [ "$kind" = windows ]; then
    for dll in dxcompiler.dll dxil.dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
        [ -f "$dist/$dll" ] || fail "$dll is missing from the package"
    done
    python=""
    for candidate in python3 python py; do
        if command -v "$candidate" >/dev/null 2>&1; then python="$candidate"; break; fi
    done
    if [ -n "$python" ]; then
        if [ "$python" = py ]; then python="py -3"; fi
        # shellcheck disable=SC2086
        $python "$root/tools/pe_imports.py" "$dist" || fail "the package does not carry every DLL its binaries import"
    else
        echo "smoke-dist: no python found; skipping the PE import check"
    fi
elif [ "$kind" = macos ] && command -v otool >/dev/null 2>&1; then
    extra=$(otool -L "$app" | tail -n +2 | grep -vE "System/Library|/usr/lib|@executable_path" || true)
    [ -z "$extra" ] || fail "non-system dynamic dependencies: $extra"
    echo "smoke-dist: no non-system dynamic dependency"
elif [ "$kind" = linux ]; then
    if command -v ldd >/dev/null 2>&1; then
        if ldd "$app" 2>/dev/null | grep -q "not found"; then
            ldd "$app" | grep "not found" >&2
            fail "the executable has unresolved shared libraries"
        fi
        echo "smoke-dist: every shared library resolves"
    fi
    # SDL2 is linked statically, so its audio backends are compiled in and the
    # binary itself names them. SDL2's CMake drops a backend whose development
    # headers are missing with no error, and on Linux the OSS driver's header is
    # the one that is always present: a static SDL2 built without
    # libasound2-dev/libpulse-dev/libpipewire-0.3-dev keeps only `dsp`, which no
    # PipeWire or PulseAudio desktop can open ("dsp: No such audio device"), and
    # the game runs silently (issue #8). Fail the release here, and not on the
    # player's machine.
    if ! LC_ALL=C grep -aqE "ALSA PCM audio|PulseAudio|Pipewire" "$app"; then
        fail "the binary carries no ALSA, PulseAudio or PipeWire SDL audio backend (issue #8)"
    fi
    echo "smoke-dist: the binary carries a Linux SDL audio backend"
fi

# --- 3. does it load and reach main? ----------------------------------------
host=other
case "$(uname -s)" in
    Darwin) host=macos ;;
    Linux) host=linux ;;
    MINGW*|MSYS*|CYGWIN*) host=windows ;;
esac
if [ "$host" != "$kind" ]; then
    echo "smoke-dist: package is $kind but this host is $host; skipping the launch check"
    echo "smoke-dist: PASS (static checks only)"
    exit 0
fi

work=$(mktemp -d 2>/dev/null || mktemp -d -t ogre-smoke)
trap 'rm -rf "$work"' EXIT
log="$work/smoke.log"

OGRE_SMOKE=1 OGRE_PREF_DIR="$work" "$app" >"$log" 2>&1 &
pid=$!
# A deadline, because a loader failure can raise a dialog and wait for a click.
# It polls in this shell: a background `sleep` watchdog outlived its subshell
# when that was killed, held the caller's output pipe open, and made every
# piped run take the full SMOKE_TIMEOUT.
deadline=$((SECONDS + timeout_seconds))
while kill -0 "$pid" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        kill -9 "$pid" 2>/dev/null || true
        break
    fi
    sleep 0.2
done
set +e
wait "$pid"
code=$?
set -e

if [ "$code" -ne 0 ]; then
    echo "smoke-dist: the app exited $code" >&2
    [ -s "$log" ] && { echo "--- output ---"; cat "$log" >&2; }
    [ -f "$work/error.log" ] && { echo "--- error.log ---"; cat "$work/error.log" >&2; }
    fail "the packaged binary did not reach main (or did not exit cleanly)"
fi
# An empty log fails. It is what a Windows build that reopens its stdio on the
# parent's console produces, and every Windows release up to session 145 did:
# the checks below were then skipped, so the session-107 capture assertion never
# ran on the platform it was written for.
[ -s "$log" ] || fail "the app exited 0 and wrote nothing to its redirected output"
grep -q "\[smoke\] main reached" "$log" || {
    cat "$log" >&2
    fail "the app exited 0 without reaching the smoke marker"
}
# The presented-frame capture must be off unless the caller set the variable.
# A stale entry made RT64 read a buffer destination as a texture on the first
# present: the v0.4.0 Windows access violation (session 107).
if [ -z "${OGRE_CAPTURE_PRESENT:-}" ]; then
    grep -q "\[smoke\] capture path: (unset)" "$log" || {
        grep "capture path" "$log" >&2 || true
        fail "the presented-frame capture is on without OGRE_CAPTURE_PRESENT"
    }
    echo "smoke-dist: the presented-frame capture is off by default"
fi
echo "smoke-dist: the binary loaded, reached main and exited 0"

# --- 4. does a crash leave a report that contains the captured log? ----------
# `OGRE_CRASH_TEST=segv` raises SIGSEGV right after the logger is armed, so the
# report must exist and must contain the [boot] line printed immediately before
# the raise. That line is only in the report if the stdout/stderr capture
# reached the ring: a handler that dies on a stdio lock, or a capture that never
# ran, leaves a report with empty sections -- exactly what a Windows report
# looked like in session 94. `error.log` goes to the executable's own directory,
# which is the package root for every package kind.
report_file="$dist/error.log"
rm -f "$report_file"
set +e
# The braces and the redirect swallow the shell's own "Segmentation fault"
# message for the intentional crash.
{ OGRE_CRASH_TEST=segv OGRE_PREF_DIR="$work" "$app" >/dev/null 2>&1; } 2>/dev/null
set -e
if [ ! -f "$report_file" ]; then
    fail "OGRE_CRASH_TEST=segv wrote no error.log"
fi
if ! grep -q "crash test OGRE_CRASH_TEST=segv" "$report_file"; then
    echo "--- error.log ---" >&2
    cat "$report_file" >&2
    fail "the crash report did not contain the captured boot log"
fi
echo "smoke-dist: the crash report captured the boot log"
rm -f "$report_file"

# --- 5. Windows: the same report from a launch with no standard handles ------
# A double-click starts the exe from Explorer with no standard handles and no
# console. main.cpp then reopens stdout and stderr on NUL as fds 1 and 2; the
# report holds the boot line only if those streams land on the descriptors that
# crash_log's capture reads (session 145). Check 4 cannot see this, because its
# launch passes the shell's handles. Explorer is the launcher used here: other
# ways to start a process with no handles (cleared handles with
# DETACHED_PROCESS, thread injection) were quarantined by Defender
# (session 119). Explorer starts a shortcut's target with Explorer's own
# environment, so the crash is requested by the --crash-test argument stored in
# a temporary .lnk. A host with no Explorer shell (a service, a hosted runner)
# skips the check.
if [ "$kind" = windows ]; then
    run_ps() { powershell.exe -NoProfile -NonInteractive -Command "$1" | tr -d '\r'; }
    explorers=$(run_ps "@(Get-Process explorer -ErrorAction SilentlyContinue).Count")
    if [ "${explorers:-0}" -eq 0 ]; then
        echo "smoke-dist: no Explorer shell on this host; skipping the no-handle launch check"
    else
        app_win=$(cygpath -w "$app")
        dist_win=$(cygpath -w "$dist")
        lnk_win=$(cygpath -w "$work/crash-test.lnk")
        run_ps "\$s = (New-Object -ComObject WScript.Shell).CreateShortcut('$lnk_win'); \$s.TargetPath = '$app_win'; \$s.Arguments = '--crash-test=segv'; \$s.WorkingDirectory = '$dist_win'; \$s.Save()"
        [ -f "$work/crash-test.lnk" ] || fail "could not write the crash-test shortcut"
        running="@(Get-Process ogrebattle64 -ErrorAction SilentlyContinue | Where-Object { \$_.Path -eq '$app_win' }).Count"
        [ "$(run_ps "$running")" -eq 0 ] || fail "$app is already running; close it and run the check again"
        rm -f "$report_file"
        # explorer.exe hands the file to the running shell and exits 1.
        explorer.exe "$lnk_win" || true
        deadline=$((SECONDS + timeout_seconds))
        until [ -f "$report_file" ] && [ "$(run_ps "$running")" -eq 0 ]; do
            [ "$SECONDS" -lt "$deadline" ] || fail "the Explorer launch wrote no error.log within ${timeout_seconds}s"
            sleep 1
        done
        if ! grep -q "crash test --crash-test=segv" "$report_file"; then
            echo "--- error.log ---" >&2
            cat "$report_file" >&2
            fail "the crash report of a launch with no standard handles did not contain the boot log"
        fi
        echo "smoke-dist: the crash report of a launch with no standard handles captured the boot log"
        rm -f "$report_file"
    fi
fi

echo "smoke-dist: PASS"
