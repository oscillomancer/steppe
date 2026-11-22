# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
"""engine.tests' portable list stays portable.

Every source in `_engine_test_sources` (host/engine/CMakeLists.txt) builds on
Linux, macOS and Windows. Only Linux is built locally, so this check catches
the commonest slips before native CI does: a POSIX-only header or call, or the
JACK test fixtures, outside a preprocessor conditional, and a variable named
after a Windows header macro. A case that needs one
is fenced (`#if defined(__linux__)`, `#if !defined(_WIN32)`, or
`#if defined(SUPPLE_TEST_JACK)`) with a one-line reason above it.

Usage: portable_sources.py <host/engine directory>
"""

import pathlib
import re
import sys

# Headers that exist on Linux and macOS but not with the MSVC runtime, and the
# JACK test fixtures (Linux only). <fcntl.h> and <sys/stat.h> exist everywhere.
HEADERS = re.compile(
    r'#\s*include\s*[<"]('
    r"unistd\.h|poll\.h|sched\.h|pthread\.h|dlfcn\.h|signal\.h|spawn\.h|"
    r"sys/(mman|resource|prctl|socket|wait|time|uio|un|select|ioctl)\.h|"
    r"supple/engine/jack\.h|fake_jack\.h"
    r')[>"]'
)
# Calls with no MSVC counterpart under these names (supple::test::processId
# and supple::test::environment, tests/file_platform.h, are the portable ones).
CALLS = re.compile(
    r"(?<![\w.>])(::)?(getpid|setenv|unsetenv|poll|pipe|mmap|munmap|ftruncate|"
    r"sched_\w+|getrlimit|setrlimit|prctl|dup2)\s*\("
)
# Names <windows.h> defines as macros (windef.h: far, near), declared as
# variables: clang-cl then sees an expression where a name should be.
WINDOWS_MACROS = re.compile(r"[\w:>]\s+(far|near)\s*[({=;,]")
OPEN = re.compile(r"#\s*(if|ifdef|ifndef)\b")
CLOSE = re.compile(r"#\s*endif\b")


def portable_sources(engine: pathlib.Path) -> list[pathlib.Path]:
    text = (engine / "CMakeLists.txt").read_text()
    block = re.search(r"set\(_engine_test_sources(.*?)\)", text, re.S)
    if not block:
        raise SystemExit("no _engine_test_sources list in host/engine/CMakeLists.txt")
    return [engine / name for name in block.group(1).split()]


def problems(path: pathlib.Path) -> list[str]:
    found = []
    depth = 0
    for number, line in enumerate(path.read_text().splitlines(), 1):
        stripped = line.strip()
        if OPEN.match(stripped):
            depth += 1
            continue
        if CLOSE.match(stripped):
            depth -= 1
            continue
        if depth or stripped.startswith("//"):
            continue
        code = stripped.split("//", 1)[0]
        if HEADERS.search(code) or CALLS.search(code) or WINDOWS_MACROS.search(code):
            found.append(f"{path.name}:{number}: {stripped}")
    return found


def main() -> int:
    engine = pathlib.Path(sys.argv[1])
    sources = portable_sources(engine)
    headers = sorted(
        p for p in (engine / "tests").glob("*.h") if p.name not in {"fake_jack.h"}
    )
    found = [line for path in sources + headers for line in problems(path)]
    for line in found:
        print(line)
    if found:
        print(
            f"{len(found)} POSIX-only or JACK use(s) outside a conditional in "
            "engine.tests' portable sources: use tests/file_platform.h or "
            "supple::waitFor, or fence the case with a one-line reason."
        )
        return 1
    print(f"{len(sources)} portable sources and {len(headers)} headers checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
