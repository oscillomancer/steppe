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
                process.kill()
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        except OSError:
            process.wait()
    return process.returncode
