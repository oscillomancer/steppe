#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
#
# A thread's alternate signal stack when its restore fails (review round 29 F4, round 31).
# The fixture's "teardown" mode aborts from an atexit handler, after the main thread's
# thread-local destructors have run. With altstack_shim.cpp preloaded the restore of the old
# (disabled) stack fails, so the destructor must keep its stack mapped: unmapping a stack that
# is still enabled makes the kernel force a SIGILL with no handler, and no report. The
# fixture prints whether the enabled stack is still mapped; the report must exist.
#
#   python3 altstack_faults.py <fixture> <shim.so> [sanitized]
#
# Exit 77: skipped (not Linux, or a sanitized build, where LD_PRELOAD does not mix).
import json, os, pathlib, signal, subprocess, sys, tempfile

def main():
    if not sys.platform.startswith("linux"): return 77
    binary, shim = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3 and sys.argv[3]: return 77
    with tempfile.TemporaryDirectory(prefix="diagnostics-altstack-") as folder:
        root = pathlib.Path(folder) / "app" / "diagnostics"
        env = dict(os.environ, LD_PRELOAD=str(shim), SUPPLE_ALTSTACK_FAIL_RESTORE="1")
        run = subprocess.run([binary, str(root), "teardown"], env=env, capture_output=True, timeout=60)
        ctx = (run.returncode, run.stderr)
        assert b"shim: restore refused" in run.stderr, ("the restore was not made to fail", ctx)
        assert b"teardown: SIGABRT handler installed" in run.stderr, ctx
        assert b"teardown: alternate stack enabled" in run.stderr, ctx
        assert b"NOT MAPPED" not in run.stderr and b", mapped" in run.stderr, ("the enabled stack was unmapped", ctx)
        assert run.returncode == 128 + signal.SIGABRT, ctx
        found = list(root.glob("*.json"))
        assert len(found) == 1, (found, ctx)
        report = json.loads(found[0].read_text())
        assert report["kind"] == "crash" and report["terminal_context"] and report["cause"] == 1, report
    return 0

if __name__ == "__main__":
    sys.exit(main())
