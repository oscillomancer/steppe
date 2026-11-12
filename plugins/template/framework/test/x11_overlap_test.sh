#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Several X11 probes at once on one display, their windows on top of each
# other, as when the suites of several products share a test display: each
# must still see its own editor's pixels, not the window over it. Exits 77
# (skipped) when there is no display.
# Usage: x11_overlap_test.sh <x11-probe> <plugin.clap> <output folder>
set -uo pipefail
probe="$1"
plugin="$2"
out="$3"
mkdir -p "$out"
pids=()
for i in 1 2 3 4 5 6; do
    "$probe" "$plugin" "$out/probe-$i.ppm" > "$out/probe-$i.log" 2>&1 &
    pids+=("$!")
done
status=0
for i in "${!pids[@]}"; do
    wait "${pids[$i]}"
    code=$?
    [ "$code" -eq 77 ] && exit 77
    if [ "$code" -ne 0 ]; then
        echo "probe $((i + 1)) of ${#pids[@]} failed:"
        cat "$out/probe-$((i + 1)).log"
        status=1
    fi
done
exit "$status"
