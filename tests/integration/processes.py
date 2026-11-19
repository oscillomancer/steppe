# SPDX-License-Identifier: GPL-3.0-or-later
"""Process cleanup shared by the integration drivers: every child they start is
ended and reaped on every path, including failures during startup."""
import os
import shutil
import subprocess
import time


def end_tree(process):
    """Windows only: ends `process` and every process it started. A crash-reporting
    supervisor (the process these tests launch) runs the real program as its child,
    and TerminateProcess on the supervisor alone leaves that child running, holding
    the files it opened (control.sock.lock). Process ids come from the parent links
    taskkill reads while the supervisor is still alive, so the child is found first."""
    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                   capture_output=True, timeout=30)


def children(pid):
    """POSIX: the live children of `pid` (a supervisor's program). Linux lists them under
    /proc; elsewhere pgrep does. Empty when neither can say."""
    found = []
    try:
        for task in os.listdir(f"/proc/{pid}/task"):
            with open(f"/proc/{pid}/task/{task}/children") as handle:
                found += [int(value) for value in handle.read().split()]
        return found
    except (OSError, ValueError):
        pass
    try:
        listed = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True,
                                timeout=10).stdout
        return [int(value) for value in listed.split()]
    except (OSError, ValueError, subprocess.SubprocessError):
        return []


def kill_children(pid):
    """POSIX: SIGKILL to each child of `pid`, so a program whose supervisor is ending hard
    does not outlive the test and leak into the next one (its endpoint lock and socket)."""
    for child in children(pid):
        try:
            os.kill(child, 9)
        except OSError:
            pass


def remove_tree(path, patience=60):
    """shutil.rmtree that, on Windows, retries while a file is still held open by a
    process that is on its way out. The retry only paces: removal either succeeds or
    the last error is raised once `patience` seconds have passed, so a process that
    never lets go still fails the test."""
    deadline = time.monotonic() + patience
    while True:
        try:
            shutil.rmtree(path)
            return
        except FileNotFoundError:
            return
        except OSError:
            if os.name != "nt" or time.monotonic() >= deadline:
                raise
            time.sleep(0.1)  # pacing only


def stop(process, timeout=10, graceful=True):
    """Ends `process` (SIGTERM when `graceful`, else SIGKILL), waits up to
    `timeout` seconds, then kills it, and always reaps it. Returns the exit
    status; never raises, so it is safe in a `finally` block."""
    if process is None:
        return None
    if process.poll() is None:
        try:
            if os.name == "nt":
                end_tree(process)  # the supervisor's child must go with it
            elif graceful:
                process.terminate()
            else:
                kill_children(process.pid)
                process.kill()
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name != "nt":
                kill_children(process.pid)  # a hung program must not outlive the supervisor
            process.kill()
            process.wait()
        except OSError:
            process.wait()
    return process.returncode
