# KeyWeaver v0.5.2

C++ CLI for converting osu!mania `.osu` charts between key counts.

Current scope:

- osu!mania `.osu` input and output
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
- v0.5.2 Preserve Tap Plus policy with key-growth budgets, hand-zone balance, and LN-heavy window additions
- v0.5.1 no-created-jack invariant across assignment, repair, expansion, and final sanitization
- v0.5.1 auto expansion default: preserve on same/lower key counts, preserve-tap-plus on higher key counts
- v0.5.2 Gesture Rail assignment for preserving detected stair, trill, and jack motifs during lane mapping

Not included: full chart editor, waveform/audio playback, BMS, DP conversion, difficulty balancing, random remix, burst echo synthesis, or DP stream splitting.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

On Windows this also builds the lightweight playtest GUI:

```bash
cmake --build build --target keyconv_gui
```

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
KeyWeaver <input.osu>
  --source <number>       source key count, optional
  --target <number>       target key count, required
  --out <path>            output file path, defaults beside input as '<stem> KeyWeaverNK.osu'
  --style <style>         direct | expand | compress | playable | faithful | training | dp
  --collision <policy>    keep | shift-nearest | merge | drop, default shift-nearest
  --compress-policy <p>   auto | preserve-strict | no-overlap-drop | no-overlap-roll | no-overlap-hybrid | training-simplify
  --distance-policy <p>   off | warn | aimod-safe | strict, default aimod-safe
  --min-gap <ms>          minimum positive object distance, default 16
  --same-lane-min-gap <ms> minimum positive same-lane distance, default 20
  --jack-preserve-policy <p> preserve-strict | preserve-playable | avoid-new-jacks | smooth-all
  --jack-window-ms <ms>   repeat/jack detection window, default 180
  --strict-jack-window-ms <ms> strict jack reference window, default 120
  --max-jack-split-lanes <n> max lanes counted as a preserved split jack, default 2
  --gesture-rail <on|off> preserve detected stair/trill/jack gesture rails, default on
  --snap-roll             snap rolled notes to timing grid, default
  --no-snap-roll          allow raw-ms roll candidates and report unsnapped rolled notes
  --snap-tolerance <ms>   snap validation tolerance, default 2
  --max-roll-ms <ms>      maximum roll distance from original time, default 64
  --expansion-policy <p>  auto | preserve | preserve-tap-plus | chord-fill | echo | training-scaffold | harder-remix | seeded-random
  --max-added-ratio <n>   max added notes as source-note ratio, default 0.15
  --max-added-per-slice <n> max added notes per source slice, default 2
  --max-added-per-measure <n> max added notes per approximate measure, default 16
  --expansion-min-gap <ms> minimum positive object gap for added notes, default 16
  --expansion-same-lane-min-gap <ms> minimum same-lane gap for added notes, default 20
  --snap-added-notes      require added notes to be timing-grid snapped, default
  --no-snap-added-notes   allow unsnapped added notes and report them
  --expansion-snap-tolerance <ms> snap validation tolerance for added notes, default 2
  --echo-policy <p>       off | stair | trill | stream | stair-trill | stair-trill-stream | auto
  --stream-echo-profile <p> conservative | balanced | training | experimental, default conservative
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
  --report <path>         write conversion report json
  --compare-policies <list> compare comma-separated policies without writing .osu output
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
```

## GUI Playtest Tool

`build/keyconv_gui.exe` is a Windows-only C++ playtest harness. It does not replace the core converter or implement chart editing; it shells out to `KeyWeaver.exe`, reads generated JSON/CSV reports, and displays a small summary/matrix for manual calibration.

GUI scope:

```text
- select input .osu
- output beside the input `.osu` by default, with optional folder override
- optional source-key override and target key
- choose expansion/compress/profile options
- run one conversion and parse report JSON
- run preserve/preserve-tap-plus/echo-balanced/training-scaffold/harder-balanced policy matrix
- open output/report and copy the generated CLI command
```

## Known Limitations

- BMS is not supported.
- DP split mode is not supported.
- The playable mapper uses greedy slice scoring, not beam search or full-song optimization yet.
- PPG-Greedy detects basic chord, jack, trill, stair, stream, burst, and LN anchor context.
- Beam search and DP-specific optimization are planned after v0.1.
- `--optimizer beam`, `--style dp`, and `--dp` are accepted as reserved options and report a fallback warning.
- Strong compression can drop or roll notes under no-overlap policies. Default hybrid compression deletes tap overflow above the target key count, but rolls overflow LNs to another safe ms when possible so their duration is preserved without overlap. Use explicit `--compress-policy no-overlap-roll` when tap overflow should be rolled instead of deleted.
- Converted osu!mania difficulty names append `KeyWeaverNK`, where `N` is the target key count, for example `KeyWeaver10K`. If `--out` is omitted, the `.osu` is written beside the input using the same marker and a numeric suffix when needed.
- The GUI mirrors this local-output default: after selecting an input `.osu`, generated `.osu`, JSON, and CSV files default to that chart's folder unless the Output field is changed.
- Default playable compression rejects near-time roll placements under `--distance-policy aimod-safe`; check `nearTimeConflicts`, `sameLaneNearConflicts`, `unsnappedRolledNotes`, `droppedByDistanceGuard`, and `rerolledByDistanceGuard` in the JSON report.
- Higher-key conversion also collapses inherited sub-16 ms cross-lane source pairs into safe same-time chords when possible, so dense source timing does not remain as visual overlap in 10K output.
- If `--expansion-policy` is omitted or set to `auto`, KeyWeaver uses `preserve` for same/lower key-count conversion and `preserve-tap-plus` when converting to a higher key count. Use explicit `--expansion-policy preserve` to disable deterministic additions on higher-key output. Check `addedNotes`, `addedByChordFill`, `addedByTrainingScaffold`, `addedNoteRatio`, and rejection counters in the JSON report.
- `--expansion-policy preserve-tap-plus` preserves original taps/LNs and adds deterministic notes. On higher-key conversion its default budget scales with key growth from 12.5% up to 25% of source objects, so 7K-to-10K targets about 1.25x total note count before safety rejects.
- Tap-plus scans 2000 ms local windows. Tap-heavy slices still add taps, while LN-heavy windows only add holds on slices that already contain a source LN. Source taps are never converted into LNs, and generated holds stay near a same-time LN anchor with collision, LN-conflict, and no-created-jack guards still taking priority.
- Higher-key `preserve-tap-plus` also balances target hand zones. For 10K, lanes 0-4 are treated as left hand and lanes 5-9 as right hand; reports include `leftHandNotes`, `rightHandNotes`, and `handBalanceRatio`.
- Generated LNs are normalized to the nearest same-time adjacent LN duration so duplicated or tap-plus long notes keep matching lengths.
- Gesture Rail is enabled by default and biases greedy assignment toward phrase-level source intent: ascending/descending stairs stay monotonic, two-lane trills stay centered on two nearby lanes, and source jacks stay same-lane or adjacent-safe. Collision, LN conflict, distance, and no-created-jack safety checks still take priority. Use `--gesture-rail off` to compare against the older lane scoring. Check `detectedStairs`, `preservedStairs`, `brokenStairs`, `detectedTrills`, `preservedTrills`, `brokenTrills`, `detectedJacks`, `preservedJacks`, `brokenJacks`, `handZoneBreaks`, `motifDirectionFlips`, `motifLaneScatterCount`, `gesturePreservationScore`, and `gestureRailEnabled` in the JSON report.
- Repeat-aware jack handling is enabled by default: source jack groups are reported, source jacks may be preserved or split by policy, source-different target repeats are hard-rejected during assignment/repair when an alternative exists, added-note candidates that would create no-source unwanted jacks are rejected, and final sanitization relanes or drops generated offenders before reporting unresolved pairs. Check `sourceJackGroups`, `preservedJackGroups`, `splitJackGroups`, `createdJacks`, `preventedJacks`, `createdJacksFromBaseMapping`, `createdJacksFromAddedNotes`, `preventedJacksByAssignment`, `preventedJacksByRepair`, `preventedJacksByExpansion`, `sanitizedCreatedJacks`, `unsolvedCreatedJacks`, `jackPreserveScore`, and `createdJackRate`.
- Expansion Composer applies explicit expansion policies in deterministic order with profile budget caps; check `expansionComposerProfile`, `targetAddedNoteRatio`, `budgetUsedRatio`, `acceptedByComposer`, `rejectedByComposerBudget`, and `rejectedByComposerSafety`.
- Playtest Calibration comparison mode does not write converted `.osu` files; it compares policy reports only and keeps core conversion defaults unchanged.
- The GUI playtest tool is a Windows harness around `KeyWeaver.exe`; it is not a realtime renderer, editor, audio player, BMS frontend, DP splitter, or beam-search UI.
- Feel metrics include `densityDelta`, `chordRateBefore`, `chordRateAfter`, `laneCoverageBefore`, `laneCoverageAfter`, `laneEntropyBefore`, `laneEntropyAfter`, `lnAnchorPressureBefore`, `lnAnchorPressureAfter`, `handSpreadAfter`, and `feelTags`.
- `--expansion-policy echo` implements same-slice stair/trill reinforcement and density-gated stream echo; check `streamEchoProfile`, `streamEchoCandidates`, `streamRawPatternCandidates`, `streamEligiblePatternCandidates`, `streamRawLaneCandidates`, `streamSafeLaneCandidates`, `streamAcceptedCandidates`, primary reject counters such as `rejectedStreamPrimaryByLocalNps`, any-reason counters such as `rejectedStreamEchoByLocalNps`, `streamEchoAddedRatio`, and `maxObservedLocalNpsAfterEcho`.
- `streamEchoCandidates` is kept for compatibility and equals `streamRawPatternCandidates`. Primary reject counts are mutually exclusive first-fail counters for accounting; any-reason counters are diagnostic counters and are not guaranteed to sum to the candidate count.
- `--expansion-policy seeded-random` is accepted as a reserved policy but does not synthesize random notes.
- The exporter regenerates `HitObjects` and preserves other sections where practical.
- Collision policy `merge` only merges duplicate same-time same-lane taps.
