# KeyWeaver v0.6.0

C++ CLI for converting osu!mania `.osu` and basic BMS-family charts between key counts.

Current scope:

- osu!mania `.osu` input and output
- BMS/BME/BML/PMS input and output MVP for playable key channels
- BMS-family input stays BMS-family output; KeyWeaver does not convert BMS to osu!mania `.osu`
- 4K to 10K-oriented SP conversion
- tap and hold note support
- direct, expand, compress, and playable lane mapping
- PPG-Greedy pattern-preserving conversion for non-direct styles
- collision detection and JSON reports
- quality report metrics for jack rate, lane entropy, pattern preservation, and playability
- reusable `keyconv::Converter` facade for embedding the core in a C++ client later
- public headers under `include/keyconv/` for adapter-ready library use
- v0.3 compress planner with no-overlap drop/roll/hybrid policies for strong key-count reductions
- v0.3.1 distance snap guard with minimum-gap validation and snap-aware rolled notes
- v0.4 deterministic expansion planner for chord-fill, training scaffold, and harder-remix note synthesis
- v0.4.1 deterministic echo for stair/trill pattern reinforcement
- v0.4.2 density-gated stream echo for low-density stream reinforcement
- v0.4.3 StreamEcho calibration profiles and diagnostics
- v0.4.4 staged StreamEcho diagnostics accounting with primary reject reasons
- v0.4.5 Expansion Composer tuning for profile-based added-note budgets
- v0.5.0 Playtest Calibration reports with FeelReport metrics and policy comparison JSON/CSV output
- v0.5.0 Windows GUI playtest harness for one-off conversion, policy matrix runs, report summaries, and copied CLI commands
- v0.5.1 GUI output/report defaults beside the selected input `.osu`
- v0.5.5 drag-and-drop GUI loading plus batch conversion: dropped files use the GUI Target field, and blank Output writes each chart beside its original file
- v0.5.5 CLI batch mode accepts multiple positional chart inputs with `--target`, writing each result beside its source chart by default
- CLI batch conversion runs charts in parallel by default, using the detected CPU thread count capped only by input count; `--jobs` can override the worker count
- CLI and GUI batch runs show percent-done progress and remaining chart count while converting
- v0.5.6 `auto-low` expansion for conservative high-key conversion
- v0.5.6 Preserve Convert mode for faithful mapping, strict source-jack preservation, and no generated notes
- v0.6.0 algorithm lock is documented at `docs/algorithm-lock-v0.6.0.md`, freezing the current generated-note, jack, LN, stream-transform, and safety contracts
- v0.6.0 10K tap-plus generation adds a stronger quarter/eighth-beat density bias than the 8K+ baseline while preserving source jack phrases
- current 8K/10K high-key mapping uses whole-board assignment and gesture hints instead of locking into 4K+4K / 5K+5K split panels
- v0.5.8 high-key generated-note presets use 10%/15%/20% low/normal/more budgets, 8K+ additions prefer 8th-beat slices with 16th-beat fallback, suppress additions on 32nd-or-faster even-key stairs, reduce outer-lane fill pressure, preserve long source jacks on one lane, and limit generated LNs to 8th-to-16th durations
- v0.5.7 Preserve Convert lane drift for adjacent safe-lane movement without adding notes, plus deterministic stream transforms (`superrandom`, `full-jitter`)
- v0.5.2 Preserve Tap Plus policy with key-growth budgets, hand-zone balance, and LN-heavy window additions
- v0.5.1 no-created-jack invariant across assignment, repair, expansion, and final sanitization
- v0.5.1 auto expansion default: preserve on same/lower key counts, preserve-tap-plus on higher key counts
- v0.5.2 Gesture Rail assignment for preserving detected stair, trill, and jack motifs during lane mapping
- v0.5.3 BMS parser/exporter MVP for `#BPM`, `#xxx03` BPM changes, visible key channels, and `#LNTYPE 1` long-note channels
- v0.5.3 BMS key-mode export headers: 4K-8K write `#4K`-`#8K` as SP, 9K defaults to `.pms`, and 10K exports as scratchless 2P channels
- v0.5.3 source-lane anchoring for stable 7K-to-10K phrase mapping across sparse same-lane repeats
- v0.5.3 low-key recreation default: high-to-low `auto` compression drops overflow objects instead of retiming every object
- v0.5.3 Target-K likeness reporting with a `kLikenessScore` / WeaveScore-style diagnostic for lane coverage, lane entropy, edge use, active-lane windows, spatial span, adjacent expansion, anchor preservation, added-ratio fit, and safety
- v0.5.3 Target-K profile builder script for KeyWeaver style profiles from curated u_e 10K osu!mania references, with broad reference scans able to include multiple authors such as `u_e` and `CircusGalop` while excluding converted charts such as `7to10C` and key-conversion tags such as `4K10C` / `5K7C`
- v0.5.5 profile-guided Adaptive Growth Budget for `preserve-tap-plus`, using 1000 ms `densityBuckets.low/mid/high/chordHeavy/jackRisk` Target-K profile windows to open or throttle local fill while keeping a global added-note cap
- v0.5.5 broad style-profile workflow validated on a 628-chart u_e + CircusGalop 10K reference set, with a sanitized reusable profile committed at `profiles/keyweaver_10k_broad_style_v1.json`
- v0.5.5 automatically loads the bundled broad 10K style profile for target-10 conversions when `profiles/keyweaver_10k_broad_style_v1.json` is beside the executable or in the working folder; `--target-profile` overrides it
- v0.5.5 algorithm lock remains historical context at `docs/algorithm-lock-v0.5.5.md`; the current frozen contract is `docs/algorithm-lock-v0.6.0.md`

Not included: full chart editor, waveform/audio playback, DP conversion, difficulty balancing, seeded random remix, burst echo synthesis, or DP stream splitting.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

On Windows this also builds the lightweight playtest GUI:

```bash
cmake --build build --target keyconv_gui
```

Release package:

```powershell
.\scripts\package_release.ps1 -Version 0.6.0
```

The package script performs a Release CMake build, runs unit/header/GUI smokes, bundles `KeyWeaver.exe`, `keyconv.exe`, `keyconv_gui.exe`, MinGW runtime DLLs, samples, scripts, profiles, and docs, then writes `dist/release/KeyWeaver-v0.6.0-win64-<timestamp>.zip` plus a `.sha256` file.

## Test

```bash
ctest --test-dir build --output-on-failure
```

You can also run the test executable directly:

```bash
build/keyconv_tests.exe
```

## Public API

External C++ clients should include the public boundary, not `src/` internals:

```cpp
#include <keyconv/converter.hpp>
#include <keyconv/chart.hpp>
#include <keyconv/convert_options.hpp>
```

Future Qwilight/TenRiff integration should adapt engine chart data into `keyconv::Chart`, run `keyconv::Converter`, then adapt the converted chart back.

```text
ExternalChart -> keyconv::Chart -> keyconv::Converter -> keyconv::Chart -> ExternalChart
```

## Usage

```bash
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --report dist/report.json
```

Options:

```text
KeyWeaver <input.osu|input.bms> [more inputs...]
  --source <number>       source key count, optional
  --target <number>       target key count, required
  --out <path>            output file path, defaults beside input using the KeyWeaver mode marker
  --style <style>         direct | expand | compress | playable | faithful | training | dp
  --collision <policy>    keep | shift-nearest | merge | drop, default shift-nearest
  --compress-policy <p>   auto | preserve-strict | no-overlap-drop | no-overlap-roll | no-overlap-hybrid | training-simplify
  --distance-policy <p>   off | warn | aimod-safe | strict, default aimod-safe
  --min-gap <ms>          minimum positive object distance, default 16
  --same-lane-min-gap <ms> minimum positive same-lane distance, default 20
  --jack-preserve-policy <p> preserve-strict | preserve-playable | avoid-new-jacks | smooth-all
  --jack-window-ms <ms>   repeat/jack detection window, default 500
  --strict-jack-window-ms <ms> strict jack reference window, default 500
  --max-jack-split-lanes <n> max lanes counted as a preserved split jack, default 2
  --gesture-rail <on|off> preserve detected stair/trill/jack gesture rails, default on
  --snap-roll             snap rolled notes to timing grid, default
  --no-snap-roll          allow raw-ms roll candidates and report unsnapped rolled notes
  --snap-tolerance <ms>   snap validation tolerance, default 2
  --max-roll-ms <ms>      maximum roll distance from original time, default 64
  --expansion-policy <p>  auto-more | auto-normal | auto-low | preserve | preserve-tap-plus | chord-fill | echo | training-scaffold | harder-remix | seeded-random
  --max-added-ratio <n>   max added notes as source-note ratio, default 0.45
  --max-added-per-slice <n> max added notes per source slice, default 2
  --max-added-per-measure <n> max added notes per approximate measure, default 16
  --preserve-convert     faithful mapping, strict source-jack preservation, no generated notes, adjacent safe-lane drift
  --preserve-lane-drift  allow Preserve Convert lane drift without the full preset
  --no-preserve-lane-drift disable Preserve Convert lane drift
  --expansion-min-gap <ms> minimum positive object gap for added notes, default 16
  --expansion-same-lane-min-gap <ms> minimum same-lane gap for added notes, default 20
  --snap-added-notes      require added notes to be timing-grid snapped, default
  --no-snap-added-notes   allow unsnapped added notes and report them
  --expansion-snap-tolerance <ms> snap validation tolerance for added notes, default 2
  --echo-policy <p>       off | stair | trill | stream | stair-trill | stair-trill-stream | auto
  --stream-echo-profile <p> conservative | balanced | training | experimental, default conservative
  --stream-transform <p>  off | superrandom | full-jitter
  --seed <n>              deterministic random seed for stream transforms, default 0
  --echo-diagnostics      print StreamEcho reject breakdown without changing conversion output
  --max-echo-ratio <n>    max echo notes as source-note ratio, default 0.08
  --max-echo-per-pattern <n> max echo notes per pattern, default 4
  --max-echo-per-measure <n> max echo notes per approximate measure, default 8
  --max-echo-per-slice <n> max echo notes per source slice, default 1
  --min-echo-pattern-length <n> minimum pattern length for echo, default 3
  --min-pattern-confidence <n> minimum PatternToken confidence for echo, default 0.70
  --echo-min-gap <ms>     minimum positive object gap for echo notes, default 16
  --echo-same-lane-min-gap <ms> minimum same-lane gap for echo notes, default 20
  --echo-max-local-nps <n> skip non-stream echo above this local NPS; StreamEcho uses profile gates, default 12
  --optimizer <kind>      greedy | beam, beam falls back to greedy in this version
  --beam-width <number>   reserved beam width option
  --epsilon <ms>          same-time TimeSlice epsilon
  --dp                    reserve DP mode, reports SP fallback in v0.1
  --dry-run               convert in memory and report only
  --batch                 treat positional chart inputs as a batch; outputs default beside each input
  --jobs <number>         batch worker count override, default detected CPU thread count
  --report <path>         write conversion report json
  --target-profile <json> use a Target-K reference profile JSON for K-likeness scoring
                          target 10 auto-loads profiles/keyweaver_10k_broad_style_v1.json when bundled
  --compare-policies <list> compare comma-separated policies without writing chart output
  --emit-feel-report      include feel metrics in comparison console output
  --emit-diff-report      include before/after diff metrics in comparison console output
  --report-csv <path>     write policy comparison csv
  --verbose               print warnings
```

## Examples

```bash
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --report dist/report_4k_10k.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --expansion-policy preserve-tap-plus --report dist/report_tap_plus.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --expansion-policy training-scaffold --out dist/simple_10k_training.osu --report dist/report_training.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --expansion-policy echo --echo-policy stair-trill --out dist/simple_10k_echo.osu --report dist/report_echo.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --expansion-policy echo --echo-policy stair-trill-stream --out dist/simple_10k_stream_echo.osu --report dist/report_stream_echo.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --expansion-policy echo --echo-policy stream --stream-echo-profile training --echo-diagnostics --out dist/simple_10k_stream_training.osu --report dist/report_stream_training.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --expansion-policy harder-remix --max-added-ratio 0.2 --out dist/simple_10k_harder.osu --report dist/report_harder.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 10 --compare-policies preserve,preserve-tap-plus,echo-balanced,training-scaffold,harder-balanced --emit-feel-report --emit-diff-report --report dist/compare.json --report-csv dist/compare.csv
build/KeyWeaver.exe samples/simple_7k_ln.osu --source 7 --target 4 --report dist/report_7k_4k.json
build/KeyWeaver.exe samples/simple_4k.osu --target 10 --dry-run
build/KeyWeaver.exe samples/simple_4k.osu --target 10 --expansion-policy auto-low --dry-run
build/KeyWeaver.exe samples/simple_4k.osu --target 10 --preserve-convert --dry-run
build/KeyWeaver.exe samples/simple_4k.osu samples/simple_7k_ln.osu --target 10 --batch
build/KeyWeaver.exe samples/simple_4k.osu samples/simple_7k_ln.osu --target 10 --batch --jobs 2
build/KeyWeaver.exe path/to/chart.bms --target 4 --dry-run
```

BMS inputs must write BMS-family outputs (`.bms`, `.bme`, `.bml`, or `.pms`). If `--out` is omitted, the original extension is kept except 9K BMS output, which defaults to `.pms`.

Already-converted inputs are skipped before conversion. KeyWeaver checks filenames plus chart `Creator` / `Version` metadata for converter markers such as `KeyWeaver10K`, `A7K`, `a10K`, `4to7c`, `7to10c`, and compact `4K10C` tags; CLI batch mode reports those files as `skipped` instead of failed.

## GUI Playtest Tool

`build/keyconv_gui.exe` is a Windows-only C++ playtest harness. It does not replace the core converter or implement chart editing; it shells out to `KeyWeaver.exe`, reads generated JSON/CSV reports, and displays a small summary/matrix for manual calibration.

The JSON/text reports include `kLikenessScore` as a 0-100 Target-K diagnostic. For 7K-to-10K work it favors keeping the 7K anchor skeleton readable while rewarding natural adjacent-lane growth, fuller 10K lane use, balanced hands, and zero created-jack/near-conflict damage. Adjacent growth gates the final score so a chart with good lane entropy but a still-7K-like skeleton does not look falsely complete. Policy comparison JSON/CSV also includes the score so future adaptive budgets can pick candidates by score gain instead of a fixed added-note percentage.

Style-profile workflow:

```bash
python scripts/build_target_k_profile.py --songs-root "<osu Songs folder>" --author u_e --author CircusGalop --author-path-prefilter --target-key-path-prefilter --target-keys 10 --out build/u_e_circusgalop_10k_profile.json
python scripts/build_target_k_profile.py --songs-root "<osu Songs folder>" --target-keys 10 --profile-name keyweaver_10k_style_v1 --profile-kind style --window-ms 1000 --curated-list scripts/u_e_10k_curated_patterns.txt --out build/keyweaver_10k_style_v1.json
build/KeyWeaver.exe path/to/source.osu --source 7 --target 10 --target-profile profiles/keyweaver_10k_broad_style_v1.json --report dist/report_7k10k_profiled.json
```

The broad profile scanner accepts osu!mania `CircleSize:10` charts whose `Creator` or `Version` contains any supplied author token and excludes converted/reference-pack markers such as `7to10`, `7to10C`, `convert`, and `KeyWeaver`. Add `--author-path-prefilter` and `--target-key-path-prefilter` for large osu! Songs folders to skip files whose path does not contain one of the author tokens or the target key token before opening metadata; when `rg` is installed, this uses `rg --files` internally for faster candidate discovery. It also rejects compact key-conversion labels like `4K10C`, `5K7C`, `7K10C`, or `4to8C` by default so those CircusGalop/u_e variants do not enter the reference set; add more `--exclude-style-token` values for similar tags. When `--curated-list` is supplied, the list entries are matched against `Title [Version]` / filename text instead, still with the same 10K and excluded-token filters. Curated profiles are style profiles, not universal 10K averages: `keyweaver_10k_style_v1` represents the selected u_e/KeyWeaver 10K hand-feel baseline.

`profiles/keyweaver_10k_broad_style_v1.json` is the sanitized committed broad profile. It keeps aggregate feature statistics and removes local Songs-folder paths. Target-10 conversions auto-load this bundled profile when it is next to the executable or in the current working folder; pass `--target-profile` to override it with a different profile.

Profile JSON includes 1000 ms window features and density buckets. It stores median/IQR-style summaries for all windows plus low/mid/high density, LN-heavy, chord-heavy, and jack-risk windows. The root `desired*` fields consumed by the current scorer are derived from these window medians. When `preserve-tap-plus` runs with `--target-profile`, KeyWeaver also enables an adaptive-growth-budget pass: the global added-note cap stays in place, but Composer pressure is based on the 1000 ms `densityBuckets.low/mid/high/chordHeavy/jackRisk` features rather than chart-level summaries.

The frozen v0.6.0 algorithm contract is in `docs/algorithm-lock-v0.6.0.md`. Treat it as the baseline for future 10K conversion tuning: any change to generated-note placement, bucket selection, local pressure, jack/LN handling, stream transforms, or safety guard behavior should update that document and the matching tests.

GUI scope:

```text
- select input .osu or BMS-family chart
- output beside the input chart by default, with optional folder override
- drag a chart file onto an already-open GUI window to convert with the current Target field
- drag files onto `keyconv_gui.exe` or `KeyWeaver.exe`; the GUI loads the first chart so Target can be set before conversion
- optional source-key override and target key
- choose streamlined GUI options: expansion `auto (more)` / `auto (normal)` / `auto (low)`, compress `auto`, stream `off` / `superrandom` / `full-jitter`, and Preserve Convert
- run one conversion and parse report JSON
- run GUI Batch from dropped charts or a selected songs/root folder, with status text showing percent done and remaining files
- run preserve/preserve-tap-plus/echo-balanced/training-scaffold/harder-balanced policy matrix
- open output/report and copy the generated CLI command
```

BMS-family inputs selected in the GUI write BMS-family outputs with the same extension family as the input, except 9K BMS output defaults to `.pms`. The GUI does not convert BMS to osu!mania `.osu`.

## Known Limitations

- BMS support is an MVP. It parses standard visible key channels, double-play key channels, `#BPM`, inline `#xxx03` BPM changes, and `#LNTYPE 1` long-note channels. During export, 4K-8K targets write SP `#PLAYER 1` plus `#4K`-`#8K`, 9K uses PMS-style 1P channels 11-19 and defaults to `.pms`, and 10K writes scratchless 2P `#PLAYER 3` channels 11-15 plus 21-25. It preserves non-playable BMS header/media lines during export, but does not yet evaluate STOP timing, random/control-flow directives, LNOBJ-style long notes, or every niche BMS extension.
- DP split mode is not supported.
- The playable mapper uses greedy slice scoring, not beam search or full-song optimization yet.
- PPG-Greedy detects basic chord, jack, trill, stair, stream, burst, and LN anchor context.
- Beam search and DP-specific optimization are planned after v0.1.
- `--optimizer beam`, `--style dp`, and `--dp` are accepted as reserved options and report a fallback warning.
- Strong compression can drop or roll notes under no-overlap policies. Default high-to-low `auto` compression uses `no-overlap-drop`, so overflow taps or holds are omitted when the target key count cannot represent the source chord/LN occupancy cleanly. This prioritizes low-key recreation over preserving every object. Use explicit `--compress-policy no-overlap-hybrid` to roll overflow holds when possible, or `--compress-policy no-overlap-roll` when tap overflow should also be rolled instead of deleted.
- Converted osu!mania difficulty names append a KeyWeaver mode marker. The base is `KeyWeaverNK`, where `N` is the target key count, and high-key auto expansion adds `(more)`, `(normal)`, or `(low)`. Stream transforms add `-sRan` or `-jitter`, for example `KeyWeaver10K-sRan (more)` or `KeyWeaver10K-jitter (low)`. If `--out` is omitted, the `.osu` is written beside the input using the same marker and a numeric suffix when needed.
- The GUI mirrors this local-output default: after selecting one input chart, generated chart/JSON/CSV files default to that chart's folder unless the Output field is changed. GUI Batch converts dropped files or a selected folder, and blank Output writes each chart beside its original file.
- Default playable compression rejects near-time roll placements under `--distance-policy aimod-safe`; check `nearTimeConflicts`, `sameLaneNearConflicts`, `unsnappedRolledNotes`, `droppedByDistanceGuard`, and `rerolledByDistanceGuard` in the JSON report.
- Higher-key conversion also collapses inherited sub-16 ms cross-lane source pairs into safe same-time chords when possible, so dense source timing does not remain as visual overlap in 10K output.
- If `--expansion-policy` is omitted or set to `auto` / `auto-normal`, KeyWeaver uses `preserve` for same/lower key-count conversion and `preserve-tap-plus` when converting to a higher key count. High-key auto presets target generated-note budgets of `auto-low` 10%, `auto-normal` 15%, and `auto-more` 20%; explicit `--expansion-policy preserve` disables deterministic additions on higher-key output. Check `addedNotes`, `addedByChordFill`, `addedByTrainingScaffold`, `addedNoteRatio`, and rejection counters in the JSON report.
- `--expansion-policy preserve-tap-plus` preserves original taps/LNs and adds deterministic notes. On 8K+ high-key conversion, generated notes prefer source slices that land on the 8th-note beat grid, use 16th-beat slices as the lower-priority fallback, avoid making both outermost lanes into the alternating trill pair, and bias additions toward whole-target mirror-lane symmetry. Target-10 conversion adds extra quarter/eighth-beat density pressure over the 8K+ baseline. Base assignment, gesture hints, and generated notes do not lock one-hand source slices to the 4K+4K / 5K+5K hand-panel split; wide-board scoring can use locally inactive lanes, globally underused lanes, and outer lanes when the target profile or built-in 8K/10K target expects broader coverage. With `--target-profile`, adaptive growth budgeting redistributes the global cap per 500-4000 ms profile window: low-density/under-expanded windows can spend more, while dense, chord-heavy, or LN-heavy windows spend less.
- `--preserve-convert` is the strict preservation preset: faithful lane mapping, source-jack strict reporting, no playable jack split accounting, and `preserve` expansion. It now enables safe adjacent-lane drift for non-jack phrases so the output is not locked to one fixed lane skeleton. Use `--no-preserve-lane-drift` to restore the older fixed feel.
- Tap-plus scans 2000 ms local windows. Tap-heavy slices still add taps, while LN-heavy windows only add holds on slices that already contain a source LN and whose anchor duration falls between the local 16th-note and 8th-note duration. Longer LN anchors are treated as tap-addition anchors instead of cloning long generated holds. Source taps are never converted into LNs, and generated holds stay near a same-time LN anchor with collision, LN-conflict, and no-created-jack guards still taking priority.
- Higher-key `preserve-tap-plus` still reports target hand-zone balance, but generated additions are free to use the full 8K/10K board when safety guards allow it. For 10K, lanes 0-4 are reported as left hand and lanes 5-9 as right hand; reports include `leftHandNotes`, `rightHandNotes`, and `handBalanceRatio`.
- Generated LNs are normalized only to same-time adjacent LN durations that still fit the 8th-to-16th generation window; generated long-hold clones are converted back to taps.
- Gesture Rail is enabled by default and biases greedy assignment toward phrase-level source intent: ascending/descending stairs stay monotonic, two-lane trills stay centered on two nearby lanes, and source jacks stay same-lane or adjacent-safe. For 8K/10K high-key output, gesture hints use the full target board instead of dual-panel hand voices. Collision, LN conflict, distance, and no-created-jack safety checks still take priority. Use `--gesture-rail off` to compare against the older lane scoring. Check `detectedStairs`, `preservedStairs`, `brokenStairs`, `detectedTrills`, `preservedTrills`, `brokenTrills`, `detectedJacks`, `preservedJacks`, `brokenJacks`, `handZoneBreaks`, `motifDirectionFlips`, `motifLaneScatterCount`, `gesturePreservationScore`, and `gestureRailEnabled` in the JSON report.
- 7K-to-10K playable mapping now uses the full 10K board for candidate zones and lane-balance pressure. Source-lane anchors and gesture shape still keep phrases coherent, but there is no hard left/right 5K split rail.
- Source-lane anchoring is part of playable assignment for higher-key output: once a source lane has established a recent target lane inside a phrase, later sparse notes from that same source lane prefer that target lane over lane-balance spreading. This keeps same-source-lane instrument feel stable unless collision, LN conflict, no-created-jack, or explicit gesture logic requires another lane.
- Repeat-aware jack handling is enabled by default with a 500 ms repeat window: source jack groups are reported, source jacks may be preserved or split by policy, long source jack phrases are kept on one target lane, source-different target repeats are hard-rejected during assignment/repair when an alternative exists, added-note candidates that would create no-source unwanted jacks are rejected, and final sanitization relanes or drops generated offenders before reporting unresolved pairs. Check `sourceJackGroups`, `preservedJackGroups`, `splitJackGroups`, `createdJacks`, `preventedJacks`, `createdJacksFromBaseMapping`, `createdJacksFromAddedNotes`, `preventedJacksByAssignment`, `preventedJacksByRepair`, `preventedJacksByExpansion`, `sanitizedCreatedJacks`, `unsolvedCreatedJacks`, `jackPreserveScore`, and `createdJackRate`.
- Expansion Composer applies explicit expansion policies in deterministic order with profile budget caps; check `expansionComposerProfile`, `targetAddedNoteRatio`, `budgetUsedRatio`, `adaptiveGrowthBudgetEnabled`, `adaptiveBudgetAverageRatio`, `adaptiveBudgetMinRatio`, `adaptiveBudgetMaxRatio`, `acceptedByComposer`, `rejectedByComposerBudget`, `rejectedByAdaptiveBudget`, and `rejectedByComposerSafety`.
- Playtest Calibration comparison mode does not write converted `.osu` files; it compares policy reports only and keeps core conversion defaults unchanged.
- The GUI playtest tool is a Windows harness around `KeyWeaver.exe`; it is not a realtime renderer, editor, audio player, BMS frontend, DP splitter, or beam-search UI.
- Feel metrics include `densityDelta`, `chordRateBefore`, `chordRateAfter`, `laneCoverageBefore`, `laneCoverageAfter`, `laneEntropyBefore`, `laneEntropyAfter`, `lnAnchorPressureBefore`, `lnAnchorPressureAfter`, `handSpreadAfter`, and `feelTags`.
- `--expansion-policy echo` implements same-slice stair/trill reinforcement and density-gated stream echo; check `streamEchoProfile`, `streamEchoCandidates`, `streamRawPatternCandidates`, `streamEligiblePatternCandidates`, `streamRawLaneCandidates`, `streamSafeLaneCandidates`, `streamAcceptedCandidates`, primary reject counters such as `rejectedStreamPrimaryByLocalNps`, any-reason counters such as `rejectedStreamEchoByLocalNps`, `streamEchoAddedRatio`, and `maxObservedLocalNpsAfterEcho`.
- `--stream-transform superrandom` deterministically randomizes every note to a safe target lane without adding notes; same-time chords are assigned distinct lanes when possible, `--seed` changes the generated lane order, and osu difficulty/file markers include `-sRan`. `--stream-transform full-jitter` offsets every note by a deterministic 1-15 ms amount for full-chart zure-style timing spread and marks output with `-jitter`; reports expose `streamTransformPolicy`, `streamTransformedNotes`, and `streamJitteredNotes`.
- `streamEchoCandidates` is kept for compatibility and equals `streamRawPatternCandidates`. Primary reject counts are mutually exclusive first-fail counters for accounting; any-reason counters are diagnostic counters and are not guaranteed to sum to the candidate count.
- `--expansion-policy seeded-random` is accepted as a reserved policy but does not synthesize random notes.
- The exporter regenerates `HitObjects` and preserves other sections where practical.
- Collision policy `merge` only merges duplicate same-time same-lane taps.
