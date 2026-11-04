# Steppe

Steppe is a digital audio workstation in which a language model can work as a
full operator, beside the person at the keyboard. Every edit, human or agent,
is a typed command on one bus, recorded in a version history that can branch
and merge. It comes with a suite of stock plugins whose parameters and patches
are described in physical units, so an agent can program them as reliably as a
person can.

![Steppe with a twelve-track session: the arrange view, the MIDI editor, the
mixer with plugins and sends, and the history sidebar showing an agent's
edits on a branch](docs/images/gui.png)

Steppe also has its own plugin framework, so you can ask an agent for a plugin
it doesn't have. Describe the effect or instrument you want, and the agent
writes it as a DSP script inside the running DAW. It loads on a track and plays
at once, and the agent measures and adjusts it while you listen. When it's
done, it exports as a CLAP plugin for any other host. The stock plugins are
written the same way.

Steppe is pre-release (0.6.0). It is used for real work on its author's
machine, but file formats and commands can still change.

## What it does

- **One command bus.** The GUI, Lua scripts, a JSON-RPC socket and MCP all
  call the same typed, schema-checked commands. Anything a person can do, an
  agent can do, and every change carries its origin in the undo history.
- **Version history with branches.** Each edit is a version. Branch, diff,
  merge (merges replay commands, with field-level conflicts), and render two
  branches for an A/B comparison. Agents propose work on a branch and the
  person accepts or rejects it.
- **Agents built in.** An MCP server drives the running app (or a headless
  engine) from Claude Code, Codex or any other MCP client, with any model,
  local ones included. A bundled skill (`skill/`) teaches agents the
  commands, units and MIDI notation.
- **Plugins from a script.** A DSP script is one C++ file in a checked
  subset of C++20 (nothing that allocates where audio runs), compiled by a
  built-in JIT the moment it is saved. Its parameters are declared in
  physical units, and it can draw an editor of its own. An edit swaps into
  the running instance with a short crossfade. A script that fails to
  compile leaves the old version playing and returns errors with the line,
  column and suggested fix. Agents work through `dsp_write`/`dsp_edit`:
  `dsp_test` reports level, distortion, frequency response and problems
  such as clipping or a note out of tune, and `dsp_preview` renders the
  editor as an image. `dsp_export` writes a ready-to-use CLAP, or a C++
  project that builds CLAP and VST3, plus AU on macOS. The DSP guides
  (`skill/guides/dsp-language.md`, `dsp-workflow.md`) teach the language,
  and the examples range from a gain to a synth, a sampler and a
  convolution reverb.
- **Measurement instead of listening.** Renders can be analyzed for loudness
  (BS.1770/R128), spectrum, pitch, envelope and stereo image, and compared
  against a reference, so an agent can check what it did.
- **Engine.** Anticipative processing for tracks that are not live, plugin
  delay compensation, CLAP and VST3 hosting with optional process isolation,
  sidechains, multi-output instruments (up to 128 channels per track),
  automation with envelopes, automation items and touch/latch/write,
  recording with input monitoring, loop and punch, takes and comping, time
  stretching. Offline renders are deterministic, and the real-time output is
  bit-identical to them.
- **Console 9 summing.** Channels and busses use a port of Airwindows'
  Console 9 encode/decode; it can be switched off per project with levels
  matched.
- **Stock plugins.** Marram (synth), Coppice (sampler), Pollard (drum
  sampler), Drumlin (drum machine), Yarrow (EQ), Sundew (compressor),
  Treeline (limiter), Gorse (transient shaper), Hickory (stereo utility),
  Sorrel (gate and expander), Teasel (saturation), Bracken (delay), Ghyll
  (reverb), Withy (chorus), Pollia (phaser), Umber (tape), Lichen
  (analyzer), Sedge (tuner), Gain and Hornbeam (guitar amp and cabinet), as
  CLAP plugins usable in any host. Each has JSON patches and exposes its
  parameters in physical units through a CLAP extension. All are written as
  DSP scripts and compiled into plugins of their own (`plugins/<name>`).
- **Text formats.** Projects, patches and FX chains are canonical JSON; MIDI
  can be written in a compact text notation.

## Installing

Downloads for [0.6.0](https://github.com/oscillomancer/steppe/releases/tag/v0.6.0):

| Platform | Installer | Portable |
| --- | --- | --- |
| Linux (x86_64) | [steppe-0.6.0-linux-x86_64.tar.xz](https://github.com/oscillomancer/steppe/releases/download/v0.6.0/steppe-0.6.0-linux-x86_64.tar.xz) (runs in place, or `./install.sh` for your user) | the same tarball |
| macOS (Apple Silicon) | [steppe-0.6.0-macos-arm64.dmg](https://github.com/oscillomancer/steppe/releases/download/v0.6.0/steppe-0.6.0-macos-arm64.dmg) | [zip](https://github.com/oscillomancer/steppe/releases/download/v0.6.0/steppe-0.6.0-macos-arm64.zip) |
| Windows (x86_64) | [steppe-0.6.0-windows-x86_64.exe](https://github.com/oscillomancer/steppe/releases/download/v0.6.0/steppe-0.6.0-windows-x86_64.exe) (per user, no admin) | [zip](https://github.com/oscillomancer/steppe/releases/download/v0.6.0/steppe-0.6.0-windows-x86_64.zip) |

Releases up to 0.5.0 were published under the old name, Quire.

The packages are unsigned. [Installing Steppe](docs/guides/installing.md)
covers user-space installation, unsigned first-launch steps and optional stock
plugins for other hosts. The [manual](docs/guides/manual.md) covers audio
devices, projects, plugins and connecting an agent over MCP.

## Building

Steppe builds with CMake, Ninja and Clang on Linux, macOS and Windows; every
dependency but SDL3, OpenGL and zlib is vendored under `third_party/`. On
Linux:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
build/release/daw/gui/steppe              # a new project
build/release/daw/gui/steppe song.json    # open a project
```

[Building Steppe](docs/guides/building.md) lists the requirements and steps for
each platform.

## Repository

| Path | Contents |
|---|---|
| `libs/` | Core libraries: JSON and schemas, semantic parameters, patches, MIDI and notation, analysis, DSP, Console 9, command bus, control protocol, Lua |
| `host/` | Plugin hosting (`clap-host`, with the bridge process), the plugin sandbox, the engine and session model |
| `daw/` | The GUI application |
| `mcp/` | The MCP adapter |
| `plugins/` | The stock plugins and their shared extension code |
| `skill/` | The agent skill: hand-written guides and generated command reference |
| `docs/` | Architecture decisions, specifications, research notes, benchmarks |
| `tools/` | CI scripts, generators, benchmarks, the evaluation harness, the REAPER check |
| `tests/` | Integration, fuzz and real-time safety tests |
| `third_party/` | Vendored dependencies with their licenses |

Start with [ARCHITECTURE.md](ARCHITECTURE.md). Design decisions are in
[docs/decisions/](docs/decisions/), component contracts in
[docs/specs/](docs/specs/), and the current state and known limits in
[STATUS.md](STATUS.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The engineering rules, for people and
coding agents alike, are in [AGENTS.md](AGENTS.md). Report security problems
privately, as [SECURITY.md](SECURITY.md) describes.

## License

Steppe's DAW code is GPL-3.0-or-later; see [LICENSE](LICENSE). Stock plugins
(`plugins/`) and their shared libraries remain MIT. The complete map and
rationale are in [ADR-0033](docs/decisions/0033-daw-and-plugin-licensing.md).
Third-party components keep their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md).
