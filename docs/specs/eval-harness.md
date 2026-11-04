# Spec: the LLM usability evaluation harness

**Status:** Implemented. In use, listening to the renders said more about
the agent than the scores did: treat the numbers as a coarse check and the
renders and transcripts as the result.
**Code:** `tools/eval-harness/`, Python 3 standard library tooling (ADR-0003).
**Runner:** Hermes with a dedicated evaluation profile (`steppe-eval`). Other
model adapters and mixing/session suites are not written yet.

The harness measures whether an agent can program plugins and write notation.
It records numeric outcomes and lists the renders that numbers cannot judge
for a listener.
A smoke run proves integration, not model quality; scripted success proves scorer
coverage, not usability.

## 1. Tasks and fixed conditions

Task files are `tools/eval-harness/tasks/<suite>/<task>.json`, schema
`supple.eval-task/1.0`. Required fields: `id`, `suite`, `prompt`, `phrase`,
`setup` (typed command/args pairs), `settings`, `render`, `score`, `limits`,
`listening`, and `recipe_index` (one-based JSON recipe number in
`skill/guides/sound-design.md`; the introductory anatomy example is number 0).
Every task plays Marram (`com.suppleaudio.marram`), loaded by its `setup`
as `in_1`.
A task's complete JSON is hashed into the report. Unknown selectors, malformed
ranges, empty profiles, and invalid limits fail before inference.

Fixed defaults: 48 kHz, 256-frame stereo rendering, 120 BPM. Descriptive
and reference phrases use one 4/4 bar (held note/chord or a short note plus rests),
with a 2-second tail cap. Notation tasks use their specified meters, no audio tail,
and one to four bars. Each repeat starts fresh; Marram's random choices are
seeded by the order of the notes, so a patch renders the same every time and
repeats measure model variability, not audio randomness. The raw captured final
patch is also retained in the report.

### 1.1 Descriptive tasks (10)

Pad, pluck bass, bell, supersaw lead, sub bass, FM electric piano, noise snare,
formant lead, acid bass and strings. A task publishes its descriptor targets to
the agent; `--blind` withholds them (same tasks and scoring), so the agent works
from the verbal description alone — the discriminating variant, since the
published targets made the first full run trivially 30/30 (2026-10-01). The dry runner sets the **current guide recipe**, rather than a stale
copied baseline; every recipe must pass its own profile. The recipes are
Marram's factory sounds of the same names; the profiles were calibrated on
Orrery's recipes and still hold. Three prompts that asked for effects (chorus,
delay, reverb) ask for the synth alone, since the final chain is the one
instrument (2026-10-21).

Profiles are simple, intentionally forgiving ranges over three descriptors:

| Family | Descriptor ranges | Recipe measurements |
|---|---|---|
| Pad | centroid 200–2200 Hz; attack 400–2400 ms; width .05–.48 | 995 Hz; 659 ms; .180 |
| Pluck bass | centroid 100–1200 Hz; attack 0–60 ms; decay20 150–1600 ms | 123 Hz; 8 ms; 532 ms |
| Bell | centroid 400–5000 Hz; attack 0–50 ms; decay20 400–3500 ms | 1038 Hz; 0 ms; 1934 ms |
| Supersaw | centroid 700–6500 Hz; voiced 30–100%; width .03–.50 | 3356 Hz; 100%; .197 |
| Sub bass | centroid 40–220 Hz; low-band level −8–0 dB; width 0–.015 | 67 Hz; −1.718 dB; 0 |
| FM e-piano | centroid 250–4000 Hz; attack 0–80 ms; decay20 300–2200 ms | 655 Hz; 0 ms; 719 ms |
| Noise snare | centroid 500–6000 Hz; attack 0–30 ms; decay20 30–600 ms | 5733 Hz; 0 ms; 118 ms |
| Formant lead | centroid 700–6000 Hz; attack 0–150 ms; median flux .001–.20 | 2184 Hz; 22 ms; .106 |
| Acid bass | centroid 200–4500 Hz; attack 0–60 ms; flatness 0–.15 | 423 Hz; 11 ms; 0 |
| Strings | centroid 300–3500 Hz; attack 100–1000 ms; width .03–.50 | 1121 Hz; 230 ms; .081 |

The pad's RMS attack includes its slow filter swell. Supersaw beating made the
global-envelope 10–90% attack read far longer than its 5 ms amp attack; that
descriptor is therefore excluded from the lead profile. A vowel's level differs
from vowel to vowel, so a wide vowel sweep moves the loudest moment, and the
measured attack, away from the onset. The listening
question checks the perceived start. Timbre numbers do not establish that a
snare is convincing, a vowel is recognizable, or a piano is pleasant. Profiles
are initial fixtures, not psychoacoustic classifiers; listening may revise them.

### 1.2 Reference tasks (10)

Tier 1: four recipe starting patches with one module changed (pad/pluck/supersaw
filter cutoff, sub-bass amp release). Tier 2: bell, e-piano and acid bass with
three changed modules (an oscillator and two envelopes). Tier 3: noise snare,
formant lead and strings, from a fresh default patch, with no starting patch
provided; their hidden patches change the recipe's filter, envelope and voice
settings, so the suite exercises analog, sub, FM, noise, the
ladder/SVF/formant filters, filter drive, envelopes, LFOs, routes and unison.
`changed_modules` names the groups changed (`filter1`, `amp`).

The harness renders the hidden patch before inference in a separate directory.
Only the WAV path is given to the agent; a tier-1/2 starting patch is applied by
the relay. Hidden patch JSON and task metadata are excluded from the agent's
prompt and relay configuration. The skill can include recipes because these
are teaching material, not the hidden modified patches.

Reference pass: optimizer loss ≤1.0, an initial operational threshold of one
aggregate normalized tolerance. Report loss, every term, and LTAS RMS dB; do not
conflate this threshold with an audible null. Exact hidden answers must produce
zero loss in dry validation.

### 1.3 Notation tasks (5)

Two-bar bass riff with rests; four-bar 3/4 waltz; one-bar triplet lead;
two-bar triads; two-bar channel-10 snare pattern. Scored dimensions: compile on
first attempted typed render, exact bars, every bar's meter, inclusive pitch
range, note attacks in every bar (including each chord note). Density is each
bar's own count of attacks, by that bar's meter, within the task's range: not the
phrase's average and not duration occupancy. A tied note counts once, in the bar
it starts. Task examples must compile and
meet their own constraints. The final answer is the **last submitted notation**,
not prose or the last successful submission. First-attempt failure remains a
failed dimension after repair. Requests hidden inside arbitrary Lua cannot be
counted reliably, so notation submission must use `render` or an explicit
`batch`/`exec` containing render. Explicitly versioned calls such as
`render@1` count as render attempts and submissions in the same order as
unversioned calls, including failures. Unknown compilation status is not a
success.
Versioned batches are unpacked before selecting the final notation. Rejected
non-object arguments and malformed batch steps remain counted tool errors;
they do not crash transcript scoring or invent notation submissions.

## 2. Runners and isolation

CLI: `run.py --suite <suite> --runner hermes|dry [--tasks ...] [--repeat N]`.
Default runner: dry; default repeat: 1. `--dry-run` forces dry regardless of
runner selection. `--sandbox`, credential/model identity sources and
Hermes executable/venv overrides are documented in the tooling README.

Hermes: one isolated `HERMES_HOME` per attempt under ignored `build/eval/`;
minimal JSON-as-YAML config, only the `sandbox` MCP toolset/server. Read only
nonsecret model identity from `~/.hermes/profiles/steppe-eval/config.yaml`
(or an explicitly supplied identity file). Default credentials are
`~/.hermes/profiles/steppe-eval/.env`, referenced through a symlink (or a same-volume hard link on Windows when
symlink privilege is unavailable); do not copy tokens,
the user's own config, sessions, memory, skills, plugins or OAuth state
into a run.
The profile name `steppe-eval` is recorded in suite and attempt metadata; the
profile chooses the model, and the report records it. Missing
profile files fail; there is **no fallback** to `~/.hermes/config.yaml`. Paths
are formed from the product slug plus `-eval`. Clear inherited `HERMES_*` overrides. Disable memory/compression, ignore private
rules, and disable lazy installation. No profile-create/use commands, shell,
filesystem, web or delegation tools are available to the evaluated agent.

Skill loading decision: explicit task preamble with SKILL.md and the relevant
guides (analysis, notation, sandbox loop, plus sound design for synth tasks).
There is no separate skill-reading tool, because it would expand the sandbox-only
tool surface. Dynamic plugin schemas come from `plugin_describe`; the guide's
currently absent generated plugin files are not fabricated. Record the entire
skill tree's SHA256 and the actual prompt.

Launch `hermes -z <prompt> --usage-file <attempt>/usage.json --toolsets sandbox
--ignore-rules`. A managed-install `--hermes-python` override invokes the same
CLI through an already installed venv, without modifying the default install or
profile. Export the isolated sessions using `hermes sessions export ... --redact`.
Record version, actual model/provider from usage, git hash and dirty flag, task
and skill hashes, tool calls/results, wall time, tokens and estimated cost
(including auxiliary usage). Unknown cost stays explicitly unknown, never zero.
Unreadable or truncated usage files mark that attempt as an error with unknown
usage; the local report retains the attempt and any earlier failure diagnostic.

Limits: per-task MCP calls and minutes. The relay rejects the first excess call
and stops; the last completed capture survives. A process-tree deadline
terminates Hermes/relay/sandbox: POSIX uses a session/process group with bounded
grace then kill; Windows uses a kill-on-close Job. A gated Python bootstrap is
assigned to the Job before executing the command, preventing descendants from
escaping during launch. Cleanup also covers descendants whose leader exited.
Excess budgets or incomplete inference cannot count as a task pass even if the
captured patch is measurable. Harness setup, capture queries and independent
scoring calls do not count as model calls.

Dry: scripted requests through the same sandbox/capture protocol, known recipe
or hidden patch, or the task's correct phrase. Every scorer is exercised without
an endpoint or credentials. Failed scripted answers fail validation. Cheap
standard-library unit tests run in CTest on Linux, macOS and Windows; opt-in integration tests exercise real
capture, limits, profile failures, reference gain invariance and all scorer paths.

## 3. Capture and production scoring

Authoritative capture decision: `relay.py` supervises `<slug>-sandbox --mcp
--record-midi --workdir <attempt>`; the slug comes from `cmake/Product.cmake`.
After every model tool response and before shutdown it queries `session.get`
and `patch.get detail=full`, atomically writing `final.json`. This includes edits
through params, batches, presets, Lua and completed optimizer jobs. It does not
parse a model's final prose or require the model to save a preset. Atomic captures
and a separate tool transcript preserve the last completed state after a kill.
Background jobs unfinished at the deadline are not applied after capture.

The final chain must contain exactly the task's Marram instrument, with a valid
semantic patch. A fresh scoring sandbox restores task settings and
renders the task phrase; synth agents cannot choose an easier scoring phrase.
Descriptive pass/fraction comes from production `profile.evaluate`. Also report
its normalized profile loss and a weighted sum of squared target distances
(the displayed distances are rounded to .001). Invalid/absent descriptors do not
silently pass. Tool error count, merges and explicit batches are efficiency
observations, not taste or arbitrary patch-validity penalties.

Reference scoring calls the sandbox's non-MCP tooling entry `eval-reference`
with both WAV paths and both note-off times. It calls the existing C++ optimizer
`analyzeMatched` and `referenceLoss`, using unrounded reports, production gain
matching, 40 harmonic frames, and all production terms (including centroid
trajectory). These helpers are shared inside the sandbox implementation, with no
new frozen command schema or MCP tool. No optimizer search runs during scoring.
The note-off metadata matters: treating a reference file's release as unknown
while measuring the candidate's release otherwise falsely penalizes identity
matches. The helper only reads WAVs/analyzes offline; it opens no audio device.
That same scored comparison is what the MCP `compare` command reports as
`loss.total`, so a reference attempt is scored by the `loss.total` it can read
against the reference and passes at ≤ the task's `max_loss`.

Compiler metadata decision: opt-in `--record-midi` writes
`renders/<id>.midi.json` (`supple.render-midi/1.0`) beside successful synchronous
notation/MIDI renders: PPQ, compiled item length, transport meter, all notation
bar meters, and note start/length/pitch/velocity/channel. It is ordinary
non-RT I/O, absent by default, and removed with its WAV on reset/rollback.
Notation structure comes from this data, never an independent notation parser.
Optimizer background candidate WAVs do not acquire these sidecars.

## 4. Reports and listening

Real runs always publish `docs/benchmarks/eval/<date>-<suite>-hermes-<model>.json`
and Markdown. Repeated suite runs on the same date get a unique run-id suffix.
JSON retains every task/repeat, failure, score, captured patch, render SHA256,
transcript path, usage and provenance; Markdown is the short review table.
`docs/benchmarks/synth-eval.md` retains one history row per published suite run.

Best/worst measured render per task (loss ordering, successful renders only)
goes to ignored `docs/benchmarks/eval/audio/<run>/`; with one repeat they are
identical. The report's Markdown lists them with each task's question for
listening. No other repository data is changed by a run. Default dry validation stays in ignored `build/eval/`;
`--publish` explicitly retains a dry benchmark, labeled as
infrastructure evidence rather than model quality.

Local transcripts, isolated profiles, usage, prompts, capture/audit files and
intermediate renders live in `build/eval/<run>/`, separate from sanitizer trees.
Retain or archive them together with the listening WAVs when sharing
reports.

Fixture launchers use Python rather than Unix executable scripts. Anonymous-pipe
readiness is bounded on POSIX and Windows, and all text files use UTF-8. Windows
credential hard links preserve file identity and ACLs without copying bytes;
cross-volume link failures stay explicit. Process-tree tests check descendant
cleanup after normal leader exit, and an echo subprocess checks stdio framing.
Failed MCP initialization also closes the client pipes and reaps its process tree.
The three real-sandbox scorer tests keep the exact SUPPLE_EVAL_INTEGRATION=1
opt-in on every platform; model quality runs require an explicitly invoked runner.
