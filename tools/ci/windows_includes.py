#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Windows SDK headers that need windows.h's types must come after it (or after
# winsock2.h, which includes it, or a first-party header that includes either).
# clang-format sorts a block of includes, so one put in the same block as
# windows.h can land above it; keep such headers in a later block.
# Usage: tools/ci/windows_includes.py FILE...   (exit 1 on a violation)
import re
import sys
from pathlib import Path

NEEDS = re.compile(
    r"#\s*include\s*<(avrt|psapi|mmsystem|winioctl|winternl|objbase|afunix|ws2tcpip|shlobj|"
    r"shellapi|dbghelp|tlhelp32|mmdeviceapi|audioclient|timeapi|processthreadsapi|synchapi|"
    r"knownfolders|commdlg|setupapi|powrprof|wtsapi32|userenv|shlwapi)\.h>")
BASE = re.compile(r"#\s*include\s*<(windows|winsock2)\.h>")
QUOTED = re.compile(r'#\s*include\s*"([^"]+)"')
known = {}


def provides(path, depth=0):
    """Whether this first-party header includes windows.h, directly or through its own."""
    path = path.resolve()
    if path in known:
        return known[path]
    known[path] = False
    if depth > 8 or not path.is_file():
        return False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if BASE.match(line.strip()):
            known[path] = True
            break
        q = QUOTED.match(line.strip())
        if q and provides(path.parent / q.group(1), depth + 1):
            known[path] = True
            break
    return known[path]


bad = False
for name in sys.argv[1:]:
    path = Path(name)
    have = False
    for number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        text = line.strip()
        if BASE.match(text):
            have = True
        elif (q := QUOTED.match(text)) and provides(path.parent / q.group(1)):
            have = True
        elif NEEDS.match(text) and not have:
            print(f"{name}:{number}: {text[text.index('<'):]} comes before <windows.h>")
            bad = True
sys.exit(1 if bad else 0)
