#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
#
# Linux collector under ptrace faults (ADR-0083, topic 4; review round 29). The
# fixture's "trace" mode runs with ptrace_shim.cpp preloaded, which makes one of the
# supervisor's ptrace calls fail or hold until the worker is in a state that is hard
# to reach otherwise. In every mode the supervisor must finish by itself and write a
# report that says what it could not read. Verdicts come from process state and the
# report, never from elapsed time: the timeout below only stops a hang this script
# does not recognise.
#
#   python3 ptrace_faults.py <fixture> <shim.so> [sanitized]
#
# Exit 77: skipped (not Linux, sanitized build, or tracing is restricted).
import json, os, pathlib, signal, subprocess, sys, tempfile, time

SKIP = 77
WATCHDOG = 120  # seconds; aborts an unrecognised hang only

def read(path):
    try: return pathlib.Path(path).read_text()
    except OSError: return None
def status(pid, tid):
    text = read(f"/proc/{pid}/task/{tid}/status")
    if text is None: return None
    out = {}
    for line in text.splitlines():
        k, _, v = line.partition(":"); out[k] = v.strip()
    return out
def tasks(pid):
    try: return sorted(int(t) for t in os.listdir(f"/proc/{pid}/task"))
    except OSError: return []
def pstate(pid):
    s = status(pid, pid); return s and s["State"][0]
def yama_scope():
    try: return int(pathlib.Path("/proc/sys/kernel/yama/ptrace_scope").read_text())
    except OSError: return 0

def run_mode(binary, shim, mode, base):
    """Run the fixture under one injection. Returns (outcome, returncode, report, events)."""
    root = base / mode / "app" / "diagnostics"
    log = base / (mode + "-events.txt")
    env = dict(os.environ, LD_PRELOAD=str(shim), SUPPLE_PTRACE_SHIM=mode, SUPPLE_PTRACE_SHIM_LOG=str(log))
    p = subprocess.Popen([binary, str(root), "trace", str(base / (mode + "-expected.json"))],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    supervisor, worker, outcome = p.pid, None, None
    started = time.monotonic()
    while outcome is None:
        if p.poll() is not None:
            outcome = "exited"; break
        if time.monotonic() - started > WATCHDOG:
            outcome = "watchdog"; break
        if worker is None:
            kids = (read(f"/proc/{supervisor}/task/{supervisor}/children") or "").split()
            if kids: worker = int(kids[0])
            continue
        # The hang this test exists for: the worker's leader is a zombie, a worker thread
        # is a zombie still traced by the supervisor, and the supervisor sits in
        # wait4(worker). Only the supervisor can reap that thread, so the wait cannot end.
        zombies = [t for t in tasks(worker) if t != worker
                   and (status(worker, t) or {}).get("State", "").startswith("Z")
                   and status(worker, t).get("TracerPid") == str(supervisor)]
        call = (read(f"/proc/{supervisor}/syscall") or "").split()
        if pstate(worker) == "Z" and zombies and call and call[0] == "61" and int(call[1], 16) == worker:
            outcome = "hang"; break
        time.sleep(0.005)  # pacing only
    if outcome in ("hang", "watchdog"):
        p.kill(); p.wait()
    errors = p.stderr.read(); p.stderr.close()
    reports = sorted(root.glob("*.json")) if root.exists() else []
    report = json.loads(reports[0].read_text()) if len(reports) == 1 else None
    events = (read(log) or "").splitlines()
    return outcome, p.returncode, report, events, errors, reports

def main():
    if not sys.platform.startswith("linux"): return SKIP
    binary, shim = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 3 and sys.argv[3]: return SKIP  # LD_PRELOAD does not mix with sanitizer runtimes
    if yama_scope() >= 2 and os.geteuid() != 0: return SKIP
    with tempfile.TemporaryDirectory(prefix="diagnostics-ptrace-") as folder:
        base = pathlib.Path(folder)
        segv = 128 + signal.SIGSEGV

        def check(mode, expect_event):
            outcome, rc, report, events, errors, reports = run_mode(binary, shim, mode, base)
            ctx = (mode, outcome, rc, events, errors, reports)
            assert outcome == "exited", ctx
            assert any(expect_event in e for e in events), ("the fault was not injected", ctx)
            assert rc == segv, ctx
            assert report is not None and report["kind"] == "crash" and report["terminal_context"], ctx
            assert not list((base / mode / "app" / "diagnostics").glob("*.run")), ("run marker left", ctx)
            assert report["threads"][0]["role"] == "fault" and report["threads"][0]["state"] == "walked", ctx
            return report["threads"], events

        # PTRACE_INTERRUPT is refused for the other threads: they stay attached and
        # running, are reported as denied, and the worker's exit is still seen.
        threads, events = check("interrupt-fail", "interrupt refused")
        assert len(threads) == 4, threads
        assert len([e for e in events if "interrupt refused" in e]) == 2, events
        others = [t for t in threads if t["state"] == "unavailable"]
        assert len(others) == 2 and all(t["reason"] == "ptrace_denied" and t["frames"] == [] for t in others), threads
        assert threads[1]["role"] == "model" and threads[1]["state"] == "walked", threads

        # A thread dies while the collector holds it stopped (the worker's own hold
        # ran out): the walk finishes, the collector reaps what it can, and the
        # supervisor is not left waiting for a thread only it can reap.
        threads, events = check("walk-freeze", "leader is a zombie, walk resumes")
        assert len(threads) == 4, threads
        assert all(t["state"] == "walked" or t["reason"] == "deadline" for t in threads), threads

        # The worker's main thread exits while the collector is attached to it: its exit
        # status must reach the supervisor from the collector, the thread is reported
        # as exited, and the other threads are cut at the deadline.
        threads, events = check("leader-exit", "leader is a zombie, collector resumes")
        assert len(threads) == 4, threads
        main = [t for t in threads if t["role"] == "model"]
        assert len(main) == 1 and main[0]["state"] == "unavailable" and main[0]["reason"] == "thread_exited", threads
    return 0

if __name__ == "__main__":
    sys.exit(main())
