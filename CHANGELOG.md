# Changelog

## 0.5.5

- Promoted the current KeyWeaver surface to `v0.5.5`.
- Added profile-guided Adaptive Growth Budget for `preserve-tap-plus`: when `--target-profile` is supplied, local profile windows now throttle or open tap-plus additions based on density, active-lane need, LN/chord pressure, and adjacent-growth pressure while keeping the global added-note cap as a hard ceiling.
- Reports expose `adaptiveGrowthBudgetEnabled`, local ratio bounds, and `rejectedByAdaptiveBudget` for profile-driven budget audits.
- Expanded the Target-K profile builder to accept repeated `--author` tokens, so broad style profiles can include both u_e and CircusGalop 10K references.
- Broad scans can use `--author-path-prefilter` and `--target-key-path-prefilter` with `rg --files` candidate discovery; compact key-conversion tags such as `4K10C` / `5K7C` / `4to8C` are excluded by default.
- Added the sanitized reusable `profiles/keyweaver_10k_broad_style_v1.json` style profile, generated from 628 u_e/CircusGalop reference charts with local Songs-folder paths removed.
- Target-10 conversions now auto-load the bundled broad style profile when `profiles/keyweaver_10k_broad_style_v1.json` is beside the executable or in the working folder; explicit `--target-profile` still overrides it.
- Relaxed the first adaptive-growth-budget pass so sparse windows can spend one local fill slot when the global tap-plus budget has room, and so 7K-to-10K profile windows do not over-throttle sections that already use the original 7 lanes well.
- Added coverage-aware tap-plus candidate scoring: locally dead lanes receive fill pressure, tap-plus can spend the second per-slice slot when the budget allows, and expansion-mode source anchors are less rigid so 8K/10K lanes do not stay visually dead.
- Raised the default high-key tap-plus growth budget by 50%: 8K/10K targets now scale up to a 37.5% added-note ratio by default, while explicit `--max-added-ratio` still overrides the cap.
- Adaptive Growth Budget now reads the profile's 1000 ms `densityBuckets.low/mid/high/chordHeavy/jackRisk` window features for Composer pressure instead of using chart-level summary fields; the Ray 7K-to-10K smoke chart now adds 420 notes with `createdJacks=0`, `nearTimeConflicts=0`, `lnConflictCount=0`, and `adaptiveBudgetAverageRatio=0.377974`.
- Documented the frozen v0.5.5 high-key expansion contract in `docs/algorithm-lock-v0.5.5.md`, including bucket selection, no-`chartSummary` Composer pressure, Ray smoke baseline, and change-control rules.
- Added `scripts/package_release.ps1` so 0.5.5 Windows release zips can be rebuilt with Release CMake, tests, GUI smoke, sample conversions, bundled profiles/scripts/samples, MinGW runtime DLLs, and SHA256 output.

## 0.5.3

- Added a basic BMS-family parser/exporter path for `.bms`, `.bme`, `.bml`, and `.pms` inputs.
- Parses playable BMS key channels, double-play key channels, `#BPM`, inline `#xxx03` BPM changes, and `#LNTYPE 1` long-note channel pairs into the shared `keyconv::Chart` model.
- Exports converted BMS notes back to BMS key channels while preserving non-playable header and media lines.
- Rejects non-BMS output paths for BMS-family inputs so BMS conversion stays BMS-to-BMS.
- Fixed the GUI conversion path so BMS-family inputs write BMS-family outputs instead of hardcoding `.osu`.
- Added BMS public headers, CLI extension dispatch, synthetic roundtrip tests, and public-header smoke coverage.
- Added BMS key-mode export handling: 4K-8K outputs write SP `#PLAYER 1` with `#4K`-`#8K`, 9K uses PMS-style 1P channels and defaults to `.pms`, and 10K writes scratchless 2P `#PLAYER 3` channels.
- Added source-lane anchor scoring for higher-key playable mapping so sparse repeats from the same 7K source lane stay on a stable 10K target lane instead of drifting only for lane-balance pressure.
- Added a 7K-to-10K dual-5K split rail so phrase motifs are recomposed inside either the left 5K panel or right 5K panel instead of being stretched across a single 7K-like lane scale.
- Added a source-panel candidate guard for 7K-to-10K non-gesture chords and sparse notes so lane-balance scoring cannot casually leak low-register source notes into the right 5K panel or high-register notes into the left 5K panel.
- Added role voice-leading for 7K-to-10K dual-5K motifs so each detected phrase prefers compact movement inside its assigned hand panel.
- Changed default high-to-low `auto` compression to drop overflow objects instead of rolling them, prioritizing low-key recreation over preserving every source object; explicit `no-overlap-roll` / `no-overlap-hybrid` remain available when object preservation is desired.
- Added Target-K likeness reporting (`kLikenessScore`) as the first WeaveScore-style scorer for lane coverage, lane entropy, edge use, active-lane windows, spatial span, adjacent expansion, 7K anchor preservation, added-ratio fit, and safety penalties. Policy comparison JSON/CSV now exposes the score so later adaptive budgets can optimize toward 10K feel instead of fixed added-note percentages.
- Added `scripts/build_target_k_profile.py`, `scripts/u_e_10k_curated_patterns.txt`, and `--target-profile` so K-likeness scoring can use either broad u_e-authored/native 10K references or the curated high-quality KeyWeaver style profile while excluding converted charts such as `7to10C`. Profile JSON now records 1000 ms window medians/IQRs and density buckets for low/mid/high, LN-heavy, chord-heavy, and jack-risk windows.
- Expanded the profile builder to accept repeated `--author` tokens, so broad style profiles can include both u_e and CircusGalop 10K references. Broad scans can use `--author-path-prefilter` and `--target-key-path-prefilter` with `rg --files` candidate discovery, compact key-conversion tags such as `4K10C` / `5K7C` / `4to8C` are excluded by default, and extra style-pack tags can be filtered with `--exclude-style-token`.
- Added the first profile-guided Adaptive Growth Budget for `preserve-tap-plus`: when `--target-profile` is supplied, local profile windows now throttle or open tap-plus additions based on density, active-lane need, LN/chord pressure, and adjacent-growth pressure while keeping the global added-note cap as a hard ceiling. Reports expose `adaptiveGrowthBudgetEnabled`, local ratio bounds, and `rejectedByAdaptiveBudget`.
- STOP timing, random/control-flow directives, LNOBJ-style long notes, and full BMS extension coverage remain future work.

## 0.5.2

- Added Gesture Rail assignment hints for detected stair, trill, and jack motifs so 7K-to-10K style expansion preserves phrase direction, two-lane trill shape, and source jack identity more consistently.
- Balanced higher-key assignment inside the matching hand zone so 7K-to-10K conversion backfills expanded left-edge lanes instead of overusing only the direct mapped lanes.
- Kept the lane-balance boost out of source jack/repeat motifs so jack identity remains stricter than density balancing.
- Changed default `preserve-tap-plus` budget for higher-key conversion to scale with key growth, capped at 25% added notes; 7K-to-10K now targets roughly 1.25x total note count before safety rejects.
- Reduced jackification during higher-to-lower key compression by making the compression planner prefer nearby lanes that do not fold different source lanes into short target repeats.
- Added 10K hand-zone balancing for higher-key assignment and tap-plus additions, treating 10K as left lanes 0-4 and right lanes 5-9.
- Added hand-zone report fields for left/right note counts and hand balance ratio.
- Made `preserve-tap-plus` scan 2000 ms local windows and generate holds only on LN-heavy slices that already contain a source LN, keeping tap-only slices as taps.
- Constrained generated tap-plus holds to same-hand, nearby same-time LN anchors so added LN lanes do not drift into unrelated columns.
- Added a final higher-key distance sanitizer that collapses inherited sub-16 ms cross-lane source pairs into safe same-time chords, fixing visible 10K overlap on LN-heavy charts such as Catch Catch.
- Normalized generated LN durations to the nearest same-time adjacent LN duration.
- Added additive gesture report fields for detected/preserved/broken stairs, trills, and jacks, hand-zone breaks, direction flips, lane scatter, and overall gesture preservation score.
- Added `--gesture-rail on|off` / `--motif-preserve on|off`; default is on.
- Fixed Windows Unicode path handling for CLI and GUI smoke paths so Korean folders/files no longer fail with `Cannot convert character sequence`.
- Made no-argument `KeyWeaver.exe` launch the bundled GUI when `keyconv_gui.exe` is beside it.
- Release packaging now includes the MinGW runtime dependency chain needed by the built executables, including `libwinpthread-1.dll`.

## 0.5.1

- Renamed the tool surface to KeyWeaver while keeping the legacy `keyconv` executable target for compatibility.
- Exported osu!mania difficulty names now append `KeyWeaverNK`, where `N` is the target key count.
- CLI conversion now writes beside the source `.osu` as `<stem> KeyWeaverNK.osu` when `--out` is omitted, avoiding existing filenames with numeric suffixes.
- Added `preserve-tap-plus` expansion policy for deterministic tap-only additions.
- Uses the full original object count, taps plus holds, as the default 12.5% added-note budget denominator.
- Preserves original holds and never generates hold notes.
- Added `addedByTapPlus` report accounting and CLI/policy-comparison support.
- Added GUI policy selection and matrix support for Preserve Tap Plus.
- Default hybrid compression now deletes tap overflow above the target key count, while preserving overflow LNs by rolling them to a safe non-overlapping ms when possible.
- Added repeat-aware Jack Guard for deterministic expansion and mapping: no-source added-note jacks are rejected, source-different target repeats are penalized during PPG scoring/repair, source jack preservation/split/created jack metrics are reported, and CLI tuning options are exposed.
- Promoted no-created-jack handling to a conversion invariant: assignment adaptively expands lanes before accepting a source-different target repeat, repair rejects jack-making shifts, final sanitization relanes or drops generated offenders, and reports now split created/prevented/sanitized jack accounting by phase.
- Simplified the GUI controls and changed GUI output/report defaults to the selected input `.osu` folder instead of `dist/gui_playtest`.
- Added `auto` expansion default: same/lower key-count conversion uses `preserve`, while higher key-count conversion uses `preserve-tap-plus`; GUI now defaults to this auto mode.
- Capped expensive compression roll candidate searches so large 7K-to-5K LN-heavy charts finish predictably.

## 0.5.0

- Added Playtest Calibration feel metrics for density delta, chord rate before/after, lane coverage/entropy before/after, LN anchor pressure, hand spread, and deterministic feel tags.
- Added `--compare-policies` for preserve/echo/training/harder policy comparison without writing `.osu` output.
- Added deterministic JSON and CSV comparison output via `--report` and `--report-csv`.
- Added a Windows C++ `keyconv_gui` playtest harness that shells out to `keyconv.exe`, parses JSON/CSV reports, displays single-convert summaries and policy matrix rows, opens output/report paths, and copies the generated CLI command.
- Added comparison-focused tests for stable feel reports, deterministic policy matrices, and safety counters.
- Kept default preserve behavior, no-RNG expansion, full-editor/BMS/DP/beam boundaries unchanged.

## 0.4.5

- Added Expansion Composer budget reporting with `expansionComposerProfile`, `targetAddedNoteRatio`, `budgetUsedRatio`, accepted count, composer budget rejects, and composer safety rejects.
- Added profile-based target added-note ratios for explicit expansion policies while keeping PreserveNoteCount at zero added notes.
- Routed `harder-remix` through staged composer caps so echo, chord-fill, and training scaffold cannot monopolize the full expansion budget.
- Kept StreamEcho conservative/default gates unchanged and preserved deterministic no-RNG behavior.
- Added composer-focused tests for preserve behavior, budget reporting, deterministic profiles, and safety output.

## 0.4.4

- Added staged StreamEcho diagnostics: raw pattern candidates, eligible pattern candidates, raw lane candidates, safe lane candidates, and accepted candidates.
- Kept `streamEchoCandidates` for compatibility and defined it as the raw pattern candidate count.
- Added mutually exclusive primary first-fail rejection counters for StreamEcho accounting.
- Reworked `--echo-diagnostics` output into stage counts, primary rejection, and any rejection sections.
- Added tests for diagnostics accounting stability, primary reject sums, diagnostics no-output-change behavior, and profile determinism.

## 0.4.3

- Added `StreamEchoProfile` with conservative, balanced, training, and experimental profile values for stream-specific density gates and budgets.
- Added `--stream-echo-profile` and `--echo-diagnostics` CLI options.
- Extended JSON/text reports with StreamEcho candidate counts and reject breakdowns for no underused lane, pattern confidence, pattern length, full same-slice chords, and lane-role rejection.
- Kept Conservative as the default and kept StreamEcho same-slice-only with no RNG.
- Added tests for diagnostics stability, profile determinism, conservative/balanced behavior, and no unsafe relaxation.

## 0.4.2

- Activated density-gated `StreamEcho` for `--echo-policy stream` and `--echo-policy stair-trill-stream`.
- Added 500ms/1000ms/2000ms local density gates, burst rejection, jack-heavy rejection, and LN-heavy rejection before stream echo candidates are accepted.
- Added deterministic underused-lane stream helper candidates with same-slice-only placement and no RNG.
- Extended quality reports with stream-specific rejection counters, stream echo ratio, and max observed local NPS after echo.
- Added tests for deterministic stream echo, low-density adds, high-density rejection, burst rejection, budget enforcement, no-conflict output, default preserve behavior, and stair/trill regression.

## 0.4.1

- Added `EchoPolicy` and CLI controls for stair/trill/stream-reserved deterministic echo.
- Implemented same-slice `StairEcho` and `TrillEcho` from detected PatternTokens with fixed candidate ordering and deterministic generated IDs.
- Wired `--expansion-policy echo` to deterministic stair/trill synthesis and added optional echo inside `harder-remix`.
- Added echo quality metrics for stair/trill/stream rule counts, echo rejection counters, and echo added ratio.
- Added tests for stair direction preservation, trill A/B preservation, echo budget enforcement, no-conflict output, and deterministic repeated output.
- Kept stream echo and seeded random synthesis reserved; no RNG is used.

## 0.4.0

- Added `ExpansionPolicy` and deterministic expansion options for note-count-preserving, chord-fill, training-scaffold, harder-remix, echo-reserved, and seeded-random-reserved modes.
- Added deterministic expansion planner module with tracked generated note IDs, fixed lane/time candidate ordering, and stable tie-break behavior.
- Implemented `DeterministicChordFill`, `TrainingScaffold`, and `HarderRemix` as a deterministic rule bundle.
- Added expansion quality metrics for added notes, rule counts, added ratio, rejection counters, deterministic flag, algorithm version, and unsnapped added notes.
- Added CLI controls for expansion policy, added-note budgets, expansion gap thresholds, and snap validation for added notes.
- Added determinism and expansion safety tests covering same-output/same-report, chord fill, training scaffold, harder remix, budgets, and no-conflict generated notes.

## 0.3.1

- Added `DistancePolicy` and minimum-gap/same-lane-gap conversion options.
- Replaced raw default roll offsets with timing-grid snap candidates for rolled compression notes.
- Added distance and snap quality metrics for near-time conflicts, same-lane near conflicts, unsnapped notes, unsnapped rolled notes, minimum positive delta, distance-guard drops, and guarded rerolls.
- Added CLI controls for distance policy, gap thresholds, snap roll, snap tolerance, and max roll distance.
- Added focused tests for rejected +8ms/+12ms roll candidates and snap-safe rolled notes.

## 0.3.0

- Added `CompressPolicy` and `--compress-policy`.
- Added no-overlap compression planning for strong reductions such as 10K -> 4K and 7K -> 4K.
- Added impossible-slice, compression drop, roll, tapify, and no-overlap guarantee metrics to quality reports.
- Added compression tests for oversized chords, full-LN occupancy, preserve-strict reporting, and hybrid no-overlap behavior.
- Limited console warning output while preserving full warning detail in JSON reports.

## 0.2.0

- Added public API headers under `include/keyconv/`.
- Added format public headers under `include/keyconv/format/`.
- Added adapter contract placeholder under `include/keyconv/adapter/`.
- Updated CMake include boundaries so `keyconv_core` exposes `include/` publicly and keeps `src/` private.
- Added a public-header smoke test target that links only against `keyconv_core`.
- Stabilized Beam/DP fallback warnings for reports and CLI verbose output.

## 0.1.0

- Bootstrapped C++20/CMake CLI project.
- Added osu!mania parser and exporter.
- Added internal chart model, lane mapping, converter core, collision detection, and JSON reports.
- Added PPG-Greedy modules for TimeSlice analysis, pattern token detection, slice assignment scoring, local repair, and quality reports.
- Added `keyconv::Converter` facade and reserved embedding-oriented options for training style, DP mode, beam optimizer, beam width, and same-time epsilon.
- Added synthetic samples and focused test harness.
