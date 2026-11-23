#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# CI-only software GL. Never installed into the product or the workstation.
set -euo pipefail
root="$GITHUB_WORKSPACE/.deps/gui-mesa-24.3.4"
archive="$root/mesa.7z"
mkdir -p "$root"
if [[ ! -f "$archive" ]]; then
  curl -fL --retry 3 https://github.com/pal1000/mesa-dist-win/releases/download/24.3.4/mesa3d-24.3.4-release-msvc.7z -o "$archive"
fi
printf '%s  %s\n' 7ebc711ad1896ac88ab21e142f1017f8ff035f0f342bdb72fbb5e2eb881ba363 "$archive" | sha256sum -c -
7z x -y "$archive" "-o$root" 'x64/*' > /dev/null
# Match the template CI: plugin embedding probes also need software OpenGL,
# and engine.tests' dsp.preview example draws offscreen in process.
# DLL search begins beside each executable, including a probe loading a CLAP.
# Deploy once per directory; never register a machine-wide driver.
while IFS= read -r -d '' destination; do
  for dll in opengl32.dll libglapi.dll libgallium_wgl.dll; do
    cp "$root/x64/$dll" "$destination/$dll"
  done
  echo "Mesa llvmpipe placed in $destination"
done < <(find build/windows/daw/gui build/windows/host/engine build/windows/host/sandbox build/windows/plugins -type f -name '*.exe' \
  ! -path '*/CMakeFiles/*' ! -path '*/_deps/*' -printf '%h\0' | sort -zu)
echo 'GALLIUM_DRIVER=llvmpipe' >> "$GITHUB_ENV"
echo 'LIBGL_ALWAYS_SOFTWARE=1' >> "$GITHUB_ENV"
