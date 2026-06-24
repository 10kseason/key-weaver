# KeyWeaver Code Architecture

This document explains how the current KeyWeaver code is wired together, where
the main behavior lives, and which files to touch for common changes.

## Big Picture

KeyWeaver has three surfaces:

```text
CLI / GUI / public C++ API
        |
        v
parser -> keyconv::Chart -> keyconv::Converter -> convertChart(...)
        |                                      |
        v                                      v
 exporter <----------------------------- ConvertResult
                                               |
                                               v
                                      JSON/text/CSV reports
```

The conversion engine is in `keyconv_core`. The CLI and public API call the core
directly. The Win32 GUI is a harness around `KeyWeaver.exe`: it builds command
lines, launches the CLI, and parses generated JSON/CSV reports for display.

## Build Targets

`CMakeLists.txt` defines the project layout:

- `keyconv_core`: core library, parsers, exporters, reports, and conversion
  logic.
- `keyconv`: CLI executable built from `src/cli.cpp`.
- `KeyWeaver`: second CLI executable with the public product name.
- `keyconv_gui`: Windows-only Win32 GUI harness in `src/gui_win32.cpp`.
- `keyconv_tests`: main regression test binary.
- `keyconv_public_header_smoke`: verifies the public headers can be consumed.

The GUI target links only Win32 system libraries plus headers. It intentionally
does not embed the core converter; it shells out to `KeyWeaver.exe`.

## Public Data Model

The public model is under `include/keyconv/`:

- `chart.hpp`: `Chart`, `ChartMeta`, timing points, notes, raw section storage,
  and warnings.
- `note.hpp`: tap/hold object representation.
- `timing.hpp`: timing point representation.
- `convert_options.hpp`: all core conversion knobs.
- `convert_result.hpp`: output `Chart` plus `ConversionReport`.
- `converter.hpp`: facade entrypoint for embedders.
- `quality_report.hpp`: enums, report fields, Target-K profile data, and
  serialized quality metrics.

External integrations should adapt their own chart format into `keyconv::Chart`,
call `keyconv::Converter::convert(...)`, then adapt the returned chart back.

## CLI Flow

The CLI lives in `src/cli.cpp`.

Main flow:

```text
main
  -> commandLineArgs
  -> optional GUI launch on Windows for no-arg / input-only double-click usage
  -> parseArgs
  -> runBatchConversion or runSingleConversion
```

`runSingleConversion(...)` is the important path:

1. Reject already-converted inputs with the reconvert guard.
2. Read the input file.
3. Choose parser by extension:
   - `.osu` -> `parseOsu(...)`
   - BMS family -> `parseBms(...)`
4. Infer or apply source key count.
5. Fill `keyconv::ConvertOptions` from CLI options.
6. Load a bundled or explicit Target-K profile when applicable.
7. If `--compare-policies` is set, run policy comparison and write JSON/CSV.
8. Otherwise call `keyconv::Converter::convert(chart, convertOptions)`.
9. Choose output path, preserving BMS-family output constraints.
10. Export `.osu` or BMS-family text.
11. Write report JSON/text when requested.
12. Print timing and summary information.

Batch mode uses the same single-conversion path per input. It adds input-list
handling, optional recursive file collection, worker threads, synchronized
console output, and skip/fail/ok summaries.

## GUI Flow

The Win32 GUI lives in `src/gui_win32.cpp`. It is a command builder and report
viewer, not a separate converter.

Key pieces:

- `AppState`: owns HWNDs, fonts, brushes, last output/report paths, and last CLI
  command.
- `createUi(...)`: creates the real Win32 controls.
- `paintUiChrome(...)`: draws the modern sidebar, cards, lane preview, report
  shell, timeline preview, and footer with GDI.
- `readToolOptions(...)`: translates control state into GUI-local
  `ToolOptions`.
- `buildSingleCommand(...)`: builds a one-chart CLI command.
- `buildBatchCommand(...)`: builds fast batch CLI command with an input-list
  file.
- `buildMatrixCommand(...)`: builds policy comparison command.
- `runProcess(...)`: launches `KeyWeaver.exe`, captures stdout/stderr, and keeps
  child processes in a kill-on-close job object.
- `parseReportSummary(...)`: reads selected JSON metrics for the redesigned
  report preview.
- `showReportSummary(...)`: formats K-likeness, added ratio, jack integrity,
  lane entropy, center bridge, warnings, and playability for the list box.
- `loadDroppedFiles(...)`: accepts chart files or folders, expands supported
  chart paths, and runs single or batch conversion.

Important behavior:

- GUI target-10 conversion enables `--ten-key-planner staged-7-14-10` and
  `--ten-k-fullfield-remix` unless Preserve Convert is active.
- Matrix comparison deliberately disables GUI full-field remix so it compares
  normal policies.
- Debug JSON controls whether normal conversion writes and parses a report.
  Without it, normal conversion writes the chart only.
- BMS-family inputs keep BMS-family outputs.

## Parser And Exporter Flow

Parsers live under `src/parser/` and public parser headers live under
`include/keyconv/format/`.

- `parser/osu.cpp`: parses osu!mania metadata, timing points, hit objects, and
  raw sections.
- `parser/bms.cpp`: parses the supported BMS-family MVP: headers, visible key
  channels, BPM changes, and `#LNTYPE 1` long-note channels.

Exporters live under `src/exporter/`.

- `exporter/osu.cpp`: regenerates `HitObjects` and preserves other sections
  where practical.
- `exporter/bms.cpp`: writes supported BMS-family output while preserving
  non-playable header/media lines where practical.

The parser output is always normalized into `keyconv::Chart`; format-specific
details should stay outside the core conversion algorithm when possible.

## Core Conversion Flow

The facade is `src/core/converter.cpp`.

`Converter::convert(...)` validates user-facing options, handles reserved
fallbacks (`beam`, DP), records warnings, then calls `convertChart(...)`.

The main algorithm is `src/core/convert.cpp`.

High-level `convertChart(...)` sequence:

1. Initialize result chart/report metadata.
2. Place source notes:
   - direct style uses lane candidates and `chooseBestLane(...)`
   - other styles use `greedyOptimizeSlices(...)`
3. Apply special high-key mapping repairs, such as 8K fast-stair vacancy logic.
4. Count created jacks from base mapping.
5. Run `applyExpansionPlanner(...)` for tap-plus, chord fill, echo, training
   scaffold, harder remix, and full-field 10K behavior.
6. Run `applyCompressPlanner(...)` for lower-key or no-overlap compression.
7. Apply stream transform pre-pass.
8. Apply collision policy.
9. Detect, relane, sanitize, or report created jack pairs.
10. Sanitize near-time overlaps and distance conflicts.
11. Apply stream transform final pass.
12. Fill final chart notes and converted difficulty name.
13. Compute quality report and final validation.
14. Attach jack, gesture, compression, distance, expansion, stream, and Target-K
    metrics.
15. Finalize K-likeness scoring with optional Target-K profile override.

Supporting modules:

- `assignment.*`: lane scoring and selection helpers.
- `optimizer.*`: PPG-greedy slice optimization.
- `mapping.*`: direct/candidate lane mapping.
- `slice.*`: same-time and near-time slice construction.
- `collision.*`: same-time and long-note conflict checks.
- `distance.*`: minimum-gap and snap validation.
- `repeat.*`: jack/repeat detection and preservation validation.
- `gesture.*`: stair/trill/jack motif detection and gesture-rail scoring.
- `expansion.*`: deterministic generated-note planners and budgets.
- `compress.*`: lower-key no-overlap drop/roll/hybrid planning.
- `pattern.*`: pattern tokens used by echo/stream logic.
- `quality.*`: quality metrics, Target-K profile scoring, and report
  finalization.
- `report.*`: JSON and text serialization.

## NK2 Report-Only Engine

`src/nk2/` contains the second-generation engine skeleton described in
`docs/nk2-design.md`.

Current files:

- `intent_graph.*`: source motif and anchor summarization.
- `layout_model.*`: target layout summaries, including the 10K
  panel/bridge/full-field model.
- `nk2_report.*`: NK2 options, report data, JSON/text serialization, and CLI
  enum parsing.
- `nk2_convert.*`: NK2 report and prototype conversion entrypoints.

In the current milestone, `--engine nk2 --nk2-mode report` remains
analysis-only. Non-report NK2 mode has a focused 7K-to-10K placement prototype:
it remaps source lanes into the 10K panel/bridge/full-field layout, preserves LN
durations, preserves source jacks as same-lane repeats, avoids new target jacks
when a safe candidate exists, and writes normal osu/BMS output through the
existing exporters. `native` and `harder` can add limited LN-end and strong-beat
support taps; each support candidate is accepted only if local safety checks
find no same-time same-lane collision, same-lane active-LN conflict, or unsafe
same-lane repeat. Placement and support ranking use layout-weighted coverage
pressure from the 10K `panel/bridge/full-field = 3/2/6` target distribution,
and NK2 reports include panel, bridge, full-field, and layout coverage scores.
The panel score is split into left/right panel spread diagnostics so real-chart
checks can catch cases where a 10K side panel looks active overall but one
lane inside the panel is under-used. Placement ranking is also motif-aware:
source jack, trill, stair, stream, chord, LN-anchor, and neutral notes receive
separate scoring adjustments while still passing through the same collision,
LN-conflict, and no-created-jack gates. NK2 reports serialize motif placement
counters for debugging. Generated support notes also carry provenance through
their internal note IDs and report counters: the ID prefix records both the
generator and anchor motif, for example `nk2-ln-ln-*` or `nk2-beat-chord-*`.
Reports additionally include source-anchor diagnostics for original notes:
`sourceAnchorTotal`, `sourceAnchorMatches`, and `sourceAnchorScore` compare
placed lanes against the direct scaled source lane while ignoring NK2-generated
support notes.
LN anchors use a freer placement score than tap anchors: duration and safety
gates stay strict, but direct-lane and source-panel bonuses are reduced so safe
repeated LNs can move through bridge, under-used full-field lanes, and occasional
opposite-panel lanes.
Neutral/stream-like original taps use the same idea at lower strength: source
jacks, trills, stairs, and chords keep their motif-preservation rules, while
ordinary original taps get weaker direct/source-panel locking, stronger
full-field under-use pressure, and a soft `left/right/right/left` panel cadence
to avoid one-sided visual clumping.
Support generation is budgeted globally and per phrase: deterministic 2000 ms
windows compute a native/harder cap from original note counts, and overflow is
reported through `rejectedByPhraseBudget` plus the aggregate budget rejection
counter.
Opposite-hand mirror support is an independent support event type. It is emitted
for safe strong-beat side-lane anchors, skipped for LN anchors, source jacks,
trills, and center-lane material, and reported with its own candidate/accepted/
rejected counters and `nk2-mirror-<motif>-*` generated note IDs.
The current profile-guided phrase scoring hook is report-only: after generation,
NK2 recomputes phrase windows, compares accepted support against the local cap,
and serializes `phraseProfile` diagnostics. It does not choose among multiple
profiles yet.
Other non-same-K pairs from 1K through 10K use `nk2-generic-nk-relane-compress`.
This generic path uses scaled source anchors plus target-lane coverage pressure.
For lower-key output it drops only notes that cannot be placed without same-time
collision, active-LN conflict, or created-jack damage; retiming and musical merge
ranking are future work. Reports expose the drop count as `droppedNotes`.
Generic 4K->5K is the current fill-note exception: it can add tap-only
strong-beat and mirror support notes in faithful mode, with phrase-local budgets
and the same collision/LN safety gates, so the added fifth lane is occupied
instead of only relaned. Its fill target is about 12% added notes globally; each
2000 ms phrase can accept 30% of local source density, clamped to 3-12 support
notes. The 4K->5K support-repeat guard is intentionally narrower than the
default NK2 500 ms guard: it uses 240 ms so dense 4K charts can gain roughly
250 support notes on the KKKK regression case without allowing immediate
new-jack damage. Other faithful NK2 paths keep source note count. Same-K input
is reported as no-op unless `--nk2-mode transform` is explicitly selected.

NKNK/NK2 real-chart automation should stay cheap while the engine is still
experimental. Use a deterministic 10% chart sample plus pinned regression
charts for routine checks. The automated gate is safety-first: reject overlaps,
LN conflicts, generated created-jacks, out-of-range lanes, broken exports, and
other obvious non-playable results. Lane distribution and authored native feel
are manual review signals, not required pass/fail checks for every iteration.

## 10K Planner And Full-Field GUI Default

There are three related 10K paths:

- `staged-7-9-10`: the default core auto planner for 7K-to-10K
  playable/training conversion.
- `staged-7-14-10`: explicit mirror-compress experiment that exposes 14-lane
  mirrored anchors before compressing into 10K candidates.
- `--ten-k-fullfield-remix`: an additional remix layer used by the GUI default
  for target 10K.

The code-level option is `ConvertOptions::tenKeyPlannerPolicy` plus
`ConvertOptions::tenKFullFieldRemix`. The GUI maps target 10K to
`staged-7-14-10` plus `--ten-k-fullfield-remix` by default, while Preserve
Convert bypasses that remix behavior.

Algorithm contracts are documented separately:

- `docs/nk2-design.md`
- `docs/algorithm-lock-v0.6.0.md`
- `docs/algorithm-lock-v0.6.1.md`
- `docs/design-10k-fullfield-remix.md`

## Report Flow

Reports are carried in `ConversionReport` and `QualityReport`.

Primary writers:

- `src/core/report.cpp`: JSON/text report serialization.
- `src/cli.cpp`: policy comparison JSON/CSV and output timing.
- `src/gui_win32.cpp`: reads report JSON for compact GUI display.

Common metrics:

- safety: collisions, LN conflicts, distance conflicts, no-overlap guarantee
- jack behavior: source/preserved/split/created/prevented/sanitized jacks
- generated notes: added count, added ratio, rejection counters, budgets
- gesture behavior: stairs, trills, jacks, hand-zone breaks
- Target-K: lane coverage, lane entropy, center bridge, split chord behavior,
  K-likeness score
- stream: transform policy, jitter count, stream echo diagnostics

If a new metric is added, update all relevant surfaces:

1. `include/keyconv/quality_report.hpp`
2. metric computation in `src/core/quality.cpp` or the producing module
3. JSON/text serialization in `src/core/report.cpp`
4. CLI comparison output when useful
5. GUI `parseReportSummary(...)` / `showReportSummary(...)` if it should be
   visible there
6. tests

## Common Change Recipes

Add a CLI option:

1. Add field to `CliOptions`.
2. Parse it in `parseArgs(...)`.
3. Copy it into `ConvertOptions` inside `runSingleConversion(...)`.
4. Include it in help text and printed summary if user-facing.
5. Add test or smoke coverage.

Add a core conversion option:

1. Add field to `include/keyconv/convert_options.hpp`.
2. Validate it in `Converter::convert(...)` if it has constraints.
3. Use it in the relevant core module.
4. Serialize/report it if behavior changes should be visible.
5. Update CLI/GUI surfaces only if users need direct control.

Add a GUI option:

1. Add control ID and HWND field in `src/gui_win32.cpp`.
2. Create the control in `createUi(...)`.
3. Read it in `readToolOptions(...)`.
4. Emit CLI flags in `buildSingleCommand(...)`, `buildBatchCommand(...)`, and
   possibly `buildMatrixCommand(...)`.
5. Add smoke coverage if command generation or report behavior changes.

Change conversion behavior:

1. Identify the core module that owns the behavior.
2. Keep safety priority intact: collisions, LN conflicts, distance guards,
   no-created-jack behavior, and source-jack preservation must stay ahead of
   style pressure.
3. Update the matching algorithm lock/design doc.
4. Add focused tests in `tests/test_main.cpp`.
5. Run the normal verification set.

## Verification

Cheap local checks:

```powershell
cmake --build build --config Release --target keyconv_gui
ctest --test-dir build --output-on-failure
build\keyconv_gui.exe --smoke samples\simple_4k.osu build\gui-smoke
```

Release/tester gate:

```powershell
.\scripts\package_release.ps1 -Version <version>
```

Use representative real charts when changing mapper behavior, jack handling,
BMS-family output paths, Target-K scoring, or GUI path behavior. Synthetic tests
are useful for exact regressions, but they are not enough for feel or large-chart
performance claims.

## Current Caveats

- The Win32 GUI uses native controls plus custom GDI painting. The lane preview
  and timeline preview are visual UI affordances, not live chart renderers yet.
- The GUI shells out to `KeyWeaver.exe`; keep GUI command generation in sync with
  CLI option semantics.
- Matrix comparison is report-only and does not write converted charts.
- BMS support is still MVP-level and intentionally keeps BMS-family output.
- Beam optimizer and DP conversion are accepted as reserved options but fall back
  to greedy SP behavior.
