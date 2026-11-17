#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# clang-format over first-party C/C++ sources.
# Usage: tools/ci/format.sh [--check]
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=(-i)
[ "${1:-}" = "--check" ] && mode=(--dry-run -Werror)
mapfile -t files < <(git ls-files --cached --others --exclude-standard -- \
  'libs/*' 'host/*' 'plugins/*' 'modules/*' 'daw/*' 'mcp/*' 'tools/*' 'tests/*' \
  | grep -E '\.(c|cc|cpp|h|hpp)$' | grep -v '/third_party/' \
  | grep -v '^plugins/template/' \
  | grep -Ev '^plugins/[^/]+/Catalog\.cpp$' \
  || true)
[ ${#files[@]} -eq 0 ] && exit 0
clang-format "${mode[@]}" "${files[@]}"
# Windows SDK headers that need windows.h must follow it, whatever the sort
# did (checked after formatting, in either mode).
python3 tools/ci/windows_includes.py "${files[@]}"
