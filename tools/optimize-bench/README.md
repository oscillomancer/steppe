# optimize-bench: does the optimizer find a reference render?

A measurement tool for `optimize.start` (spec: `docs/specs/optimizer.md`). It exists because the first evaluation runs showed agents
running 291-evaluation searches that never improved on their starting patch,
while a one-parameter search from a fresh session found the answer.

For every `synth-reference` task the benchmark

1. loads the task's synth (Marram) and sets its `starting_patch`, as the
   evaluation harness does,
2. renders the task's `hidden_patch` as the reference (`rd_1`),
3. runs `optimize.start` against that reference with a fixed evaluation budget,
   for each mode and search seed,
4. records the initial and best loss, evaluations, wall time, failed
   candidates, the stop reason, and whether the best loss reached the task's
   `score.max_loss`.

## Modes

- `auto` — every tunable parameter of the patch's present modules, exactly the
  optimizer's own `"auto"` (continuous, modulatable, active, minus `voice.*`,
  the output level, pans, envelope curves/delay/hold, velocity, LFO fades).
- `module` — the subset of that list belonging to the modules the task lists in
  `changed_modules`, passed as explicit keys: `amp` means the `amp.*`
  parameters.
  Explicit keys keep the full parameter range (only `"auto"` narrows ranges), so
  this mode isolates the search algorithm from the range choice.

Tier-3 tasks (`changed_modules` empty, `starting_patch` empty) are recorded as
not applicable: they start from no structure, so the optimizer has nothing to
tune — a structure task, not a number task.

## Usage

```sh
# Baseline, four tasks in parallel (the machine is shared: keep jobs ≤ 6)
python3 tools/optimize-bench/bench.py --label 2026-10-01-baseline --jobs 4

# One variant, a few tasks, fewer evaluations while iterating
python3 tools/optimize-bench/bench.py --label sigma-0.1 --tasks sub-bass pad \
    --seeds 1 --evaluations 100

# A comparison table over several reports
python3 tools/optimize-bench/summary.py docs/benchmarks/optimizer/2026-10-01-*.json
```

Each run writes `<label>.json` (full per-run records plus the aggregate) and
`<label>.md` (the tables) into `docs/benchmarks/optimizer/`, and a raw
per-run JSONL into `build/optimize-bench/<label>.jsonl`. Both modes × three
seeds × ten tasks at 300 evaluations takes roughly 10 minutes at `--jobs 4`;
`--evaluations` and `--tasks` are the dials for a quick iteration.
Task failures remain in the JSON and Markdown reports and make the command
exit nonzero. A completed search that misses its loss target is still a valid
measurement, as are the documented not-applicable and no-parameter skips.
Comparison tables label each mode and use the median for any number of seeds.

A variant is a code change in the product: rebuild only the sandbox
(`cmake --build --preset dev --target steppe-sandbox -j6`) and run with a new
`--label`, so every table row names the build that produced it. The chosen
defaults belong in `docs/specs/optimizer.md` (Amendments); `summary.py` writes the comparison table.

The tool imports the evaluation harness's helpers (`tools/eval-harness/common.py`)
rather than duplicating a sandbox client, and it never publishes eval reports:
everything lands under `build/` except the JSON/Markdown it is asked to write.

## Notes from building it

- **The optimizer is the authority on which parameters are tunable.** The
  benchmark reads the `"auto"` key list back from a one-generation probe run
  instead of deriving it from `plugin.describe`: describe's `active` flags were
  then the schema captured at *load* and did not follow a patch that added a
  module, while the optimizer read the instance's live schema (describe has
  followed the patch since).
- **Runs are deterministic, their trajectory is order-sensitive.** The same
  configuration and seed give bit-identical losses (verified three times), but
  the CMA stream is drawn per coordinate, so reordering the `params` list changes
  the search. Reports record the key order with every run.
- **A mode can be skipped, not failed:** a task whose changed modules have no
  tunable parameters records a skip, and tier-3 tasks (empty `starting_patch`)
  are not applicable at all.
- Timing is machine-dependent; compare *losses* between reports, not seconds.
