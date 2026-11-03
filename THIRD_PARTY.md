# Third-party components

Every dependency is vendored under `third_party/<dir>/` with its license file,
and declared in `third_party/manifest.json` (machine-readable: license SPDX
expression, version, upstream, pinned commit, purpose, linkage).
`tools/license-check/check.py` runs as part of every build and fails when
anything is undeclared, not on the allow-list, missing its license file, or
not described here. Policy: [ADR-0003](docs/decisions/0003-toolchain-and-dependencies.md).
The gate also checks existing SPDX tags in first-party source, build and
installer scripts, their templates, and GitHub workflows.

Allowed for bundled code: MIT, MIT-0, BSD-2/3-Clause, ISC, Zlib, Apache-2.0,
BSL-1.0, CC0, Unlicense (OFL-1.1 for fonts). The ASIO SDK is the sanctioned GPL-3.0-only exception, Windows DAW only,
never plugins (ADR-0033). LAME is the sanctioned LGPL-2.0-or-later exception,
DAW only, never plugins or the MIT libraries (ADR-0055 §8). Other GPL dependencies need a DAW-only manifest
declaration and maintainer approval; AGPL is forbidden. LGPL/MPL need the maintainer's explicit approval. Dynamically linked OS libraries
(libc, ALSA, JACK/PipeWire client libs, X11/Wayland/GL) are acceptable.

| Name | License | Version / commit | Purpose | Linkage |
|---|---|---|---|---|
| asiosdk | GPL-3.0-only AND BSD-3-Clause (selected GPL option) | 2.3.4 / 2025-10-15; archive SHA-256 `d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca` | ASIO backend; common and Windows host sources only | static, Windows DAW only, never plugins |
| libogg | BSD-3-Clause | 1.3.6; tarball SHA-256 `5c8253…1061` (Xiph's SHA256SUMS) | Ogg container for Ogg Vorbis render outputs; `src/` and headers, with our `config_types.h` | static, DAW only (libs/audio-codecs) |
| libvorbis | BSD-3-Clause | 1.3.7; tarball SHA-256 `b33cc4…954b` (Xiph's SHA256SUMS) | Ogg Vorbis render outputs (vorbisenc) and their delivery checks (vorbisfile); `lib/` sources and headers | static, DAW only (libs/audio-codecs) |
| lame | LGPL-2.0-or-later | 3.100; tarball SHA-256 `ddfe36…da1e` (SourceForge's) | MP3 render outputs; the libmp3lame encoder sources and `lame.h`, with our `config.h`; no mpglib decoder, frontends or assembly | static, DAW only (libs/audio-codecs), never plugins or MIT libraries |
| clap | MIT | 1.2.10 / `195b42a` | CLAP plugin ABI headers for hosting and for the stock plugins | headers |
| doctest | MIT | 2.5.3 / `2d0a935` | Unit-test framework; tests only, never shipped | tests |
| airwindows-console9 | MIT, © 2018 Chris Johnson | `d22a25b7f7c0c8c05e9f3ae480e7f1da9769f49d` | Airwindows Console 9 Channel/Buss reference for the DSP null test; mono AU kernels retained as reference text | test-only, never linked into a shipped binary |
| fast_float | MIT (selected option) | 8.3.0 / `b0ab987` (2026-09-15; tarball SHA-256 `904859…b13f`) | Shared float/double parsing in core-base; MIT libraries, stock plugins and source exports | headers |
| dr_libs | Unlicense OR MIT-0 | dr_wav 0.14.5 / `fa931f3` | WAV read/write; FLAC and MP3 decode | static (header) |
| pocketfft | BSD-3-Clause | cpp branch / `c90e55b` | FFT for offline analysis and analyzer displays | headers |
| signalsmith-stretch | MIT | 1.4.0 / `a670068` (tarball sha256 `077235…f284`) | Pitch-preserving offline media variants and streaming master-playrate source conversion; [portable RNG/reset patch](cmake/patches/signalsmith-stretch-streaming-reset.patch) | headers |
| signalsmith-linear | MIT | 0.6.4 / `de55e6a` (tarball sha256 `2cb10d…a56`) | STFT/FFT layer Signalsmith Stretch is built on (its pinned dependency) | headers |
| vst3sdk | MIT | v3.8.0_build_66 / `9fad977` | VST3 hosting ABI, Linux/macOS/Windows module loaders, event and parameter helpers | static |
| AudioUnitSDK | Apache-2.0 | AudioUnitSDK-1.1.0 / `53a9a20` | Existing macOS stock-plugin AUv2 wrappers, clap-wrapper pin | static |
| lua | MIT | 5.4.9 (lua.org tarball, sha256 `2335b6c5…`; upstream `src/` unmodified) | The embedded scripting language behind `libs/lua-bus` and the `script.run` command (ADR-0006); built as a static library with only the core and sandbox libraries compiled | static |
| mir | MIT | 1.0.0 / `477d820` (tarball sha256 `fdc934…4055`; two local patches: the register allocator's `cmake/patches/mir-spilled-reg-reload.patch` and the x86-64 float comparison's `cmake/patches/mir-float-compare-rex.patch`) | JIT compiler and interpreter behind DSP scripts (ADR-0032); only the core, generator and interpreter sources are vendored | static |
| clap-wrapper | MIT (bundles fmt: MIT; PreSonus gain-reduction header: public domain) | 0.16.0 / `1cca996` (template pin) | Builds template-derived plugins (CLAP; VST3/AUv2 when enabled) | static, plugins only |
| pugl | ISC (a few build files 0BSD OR ISC) | 0.5.7 / `b763714` (template pin) | Plugin editor windowing (template GUI stack) | static, plugins only |
| nanovg_metal | MIT | template pin `1e42439` | macOS-only plugin editor renderer (the template's default on Apple); vendored unmodified, never built on Linux or Windows | static, macOS plugins only |
| nanovg | Zlib (bundled stb_truetype/stb_image: public domain or MIT) | `ce3bf74` (template's pin) | Vector rendering for the DAW GUI | static |
| nanosvg | Zlib | `239e102` (template's pin) | SVG parsing for editors of plugins built on the plugin template; only the parser header is vendored, unmodified | static, plugins only |

Packaging redistributes the **SDL3-runtime** (Zlib; license in
`third_party/sdl3-runtime/LICENSE.txt`). Linux CI builds 3.2.30; other
platforms use the runner's SDL3 package. Windows installers contain the
**NSIS** 3.x stub and uninstaller (Zlib; `third_party/nsis/LICENSE.txt`).
They explicitly select the zlib compressor: NSIS's separately licensed LZMA
compressor is not used. NSIS is a build tool on disposable runners.
Windows also redistributes **zlib-runtime** 1.3.2 from vcpkg (Zlib;
`third_party/zlib-runtime/LICENSE.txt`); its configured version is recorded
beside that notice in the package's `VERSION.txt`. Linux/macOS use OS zlib.
Linux package programs and stock CLAPs statically link hidden libstdc++/libgcc runtimes, with the
GCC GPLv3 runtime exception and actual system-package copyright notices;
no shared C++ runtime is bundled or required by the packaged code (ADR-0049). The
release container establishes their minimum ABI. No JACK library is bundled.

Other system libraries (dynamically linked, not bundled): zlib on Linux/macOS (Zlib; PNG snapshots and dsp.preview's pictures), libjack
from pipewire-jack (MIT, dlopen'ed), libpipewire-0.3 (MIT), alsa-lib (LGPL,
OS library, dynamic only — explicitly permitted by policy), libX11/libGL, and libEGL (dlopen'ed by
plugins of the template to draw an editor without a window).

Fonts are assets, not code: SIL OFL-1.1 fonts (Inter, IBM Plex, Fraunces — the fonts the plugin template ships) are permitted for bundling; see
ADR-0003. The DAW GUI bundles Inter (Regular, SemiBold) and IBM Plex Mono (Regular,
Medium) under `assets/fonts/` with their OFL-1.1 license texts.

The vendored clap-wrapper has three pre-applied template patches recorded in
`third_party/manifest.json`, with copies under `cmake/patches/`: all automation
points, a timer floor, and `clap-wrapper-vst3-self-handle.patch` (template commit
`3d32b8d`). The last releases the Linux self-module lookup handle immediately
and resolves `clap_entry` in that image, avoiding cross-plugin binding and
unmapping from inside its own static destructor. Plugin builds validate these
patches without mutating the shared vendored tree.

Template-derived stock plugins build offline against the vendored clap,
clap-wrapper, Pugl and NanoVG trees above (ADR-0011). The VST3 SDK hosting subset
is vendored for the host; stock-plugin VST3 builds remain opt-in. Plugin fonts (Inter, IBM
Plex, Fraunces; OFL-1.1) travel with each plugin copy with their license files.

First-party content, not third-party: Coppice's factory samples
(`plugins/coppice/samples/*.flac`, embedded in the plugin) are rendered
from Marram by `tools/factory-samples/render.py` and dedicated to the public
domain under CC0 1.0 (`plugins/coppice/samples/LICENSE`); so are Pollard's
factory hits (`plugins/pollard/samples/*.flac`, `plugins/pollard/samples/LICENSE`).

Development tool (not linked, bundled, or used at runtime):
`free-audio/clap-validator` 0.4.1, tag `0.4.1` at
`152b9823e992d782c5c1fd33bca0295478b919aa`, MIT license. The source,
license, Cargo packages and binary live in the user's tool cache outside this
repository; `tools/validator/build.sh` pins and builds it for optional CLAP
conformance checks.

## LAME notices

LAME 3.100 is under the GNU Library General Public License version 2 or
later (`third_party/lame-3.100/COPYING`; the upstream LICENSE and README are
kept beside it). The maintainer approved it on 2026-11-03 (ADR-0055 §8). It is
linked statically into the GPL-3.0-or-later DAW only, where the DAW's own
source offer covers it; the vendored files are unmodified and the only
addition is `config/config.h`, the build configuration. It never enters
`plugins/` or the MIT libraries, and `tools/license-check` allows it by name.
MP3 encodes carry LAME's own encoder tag ("LAME3.100") in the LAME/Xing
header, as upstream writes it.

## ASIO SDK notices

The identical root and common LICENSE.txt files offer the proprietary
Steinberg ASIO license or GPL version 3, without an "or later" grant.
They apply only to files referencing them; embedded licenses govern other
files. Quire selects GPL-3.0-only. Host helpers carry BSD-3-Clause notices:
source distributions retain copyright, conditions and disclaimer; binary
distributions reproduce them in documentation or accompanying materials,
and endorsement requires permission. Preserve those notices when shipping.
The proprietary option requires a signed agreement before publication and
written permission to redistribute the SDK; Quire does not select it.

The unmodified SDK README says branding is optional under GPLv3. If used,
ASIO trademarks and logos must follow the included usage guidelines: logos
unaltered, only in product-related contexts; no ASIO-derived product/company
names, branding of incompatible products or offensive associations. It says
logo non-compliance does not affect GPL rights but may violate trademarks.
The license recommends those guidelines. No logo artwork is bundled here.
The two license files, README, proprietary agreement and usage guidelines
are preserved alongside the subset. `cmake/PortAudio.cmake` integrates the
subset into the Windows DAW's PortAudio backend.

PortAudio v19.7.0 (2021-04-06), commit
147dd722548358763a8b649b3e4b41dfffbcfbb6, has its unmodified native source/header subset vendored under
`third_party/portaudio-19.7.0`; MIT license in LICENSE.txt. It is built only for
DAW device audio on macOS (CoreAudio) and Windows (WASAPI + ASIO). Windows's
ASIO-enabled static build also contains the separately licensed ASIO SDK
GPL-3.0-only subset; neither dependency enters plugin binaries. PortAudio's
non-binding requests are preserved in its license. Linux retains JACK.

The Signalsmith Stretch patch fixes the default random sequence and uniform
sample conversion across standard libraries, in addition to allocation-free
seeded resets. It deliberately changes seeded phase variation from the previous
standard-library RNG. Plain `reset()` retains the current sequence; explicitly
supplied custom engines retain upstream distribution behavior.
