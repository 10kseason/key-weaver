# E+ local validation

Date: 2026-11-01. Windows x64, GNU C++ 15.1.0, Ninja, Release, ONNX disabled.
Base repository commit: `2c5a035c3f6728f4f94748f90e6d5740fd5351d2`.
These results describe the local source build, not the published 1.3.1 binaries.

## Algorithm v2: source-lane pinning follow-up

The later user request applies to all supported source key counts, not only
4K. Validation used a separate `build-e-unpinned` build, retaining the previous
binary and comparison outputs. The full ten-suite CTest run, GUI smoke and
pre-change default-E byte comparison passed. The E+ executable now has thirteen
test groups, including direct and full-converter tests for every source count
1–18, a balanced ten-lane field that hides pinned originals, and a dense 10K
boundary where ownership changes must be rejected by the 500ms guard.

On the same local native 4K chart, original-source-to-target concentration
fell from **0.324042 in E+ v1 to 0.118562 in v2** (0.1 is uniform 10-lane use).
This excludes generated notes. Each of the four original lanes used all ten
target lanes. Peak share in any one target lane, by original lane:

| Original lane | E+ v1 | E+ v2 |
|---|---:|---:|
| 1 | 40.00% | 15.76% |
| 2 | 38.85% | 13.67% |
| 3 | 46.64% | 22.39% |
| 4 | 42.33% | 22.70% |

V2 retained 2,743 heads / 108 holds, used all ten lanes in every nonempty
2-second window, and had zero 8-second hotspots. Mean 2-second concentration
was 0.135212. Compared with plain E, 1,733 notes moved; 66 long chains were
segmented and 317 safe pair exchanges were accepted. Work was 1,287,356 safety
note visits, without budget exhaustion or rollback. Original-source safety
passed with zero collisions, LN conflicts, created repeats, retiming or changed
LN durations. Preserved repeat edges decreased from 1,736 to 1,611: long-chain
continuity at segment boundaries is intentionally no longer a v2 invariant.

Serialized non-lane event fields matched v1; repeated v2 chart/report/diagnostic
bytes matched; default E chart/report bytes matched the prior E build. An
independent serialized-chart counter matched the hotspot populations. Current
chart-folder files were read and hashed without modification. Playability and
the feel of long-repeat transitions still require human comparison.

The following sections record the earlier v1 run and are historical.

## Automated checks

- Release CLI, GUI, core and test targets built successfully in a separate
  `build-e-fullfield` directory. The earlier executable was not overwritten.
- All ten registered CTest suites passed. The E+ core executable has ten
  focused groups, including native 10K, absent version metadata and deterministic
  work-budget exhaustion on 6,000 heads. The CLI suite covers automatic/explicit
  E+, JSON identity, unused default filenames, dry-run, rejected combinations,
  parallel batch, same-key export and BMS-family output.
- GUI `--smoke` with `samples/simple_7k_ln.osu` passed. Its E+ assertions cover
  single/batch flags, prerequisites, output suffix, target restrictions and
  unchanged previous selections. The new GUI also launched successfully.
- The hotspot CLI suite passed with the executable captured before diagnostic
  changes as `BASELINE_EXE`; default E chart and existing report bytes matched.
- `git diff --check` passed. The pre-existing GCC maybe-uninitialized warning
  in Classic `assignment.cpp` remains; no new compiler warning was observed.

## Local chart comparison

One user-selected native 4K chart was converted with explicit E and E+ at 10K.
The chart/audio are local and are not included in this source document.

| Observation | E | E+ |
|---|---:|---:|
| Output note heads | 2,743 | 2,743 |
| Hold heads | 108 | 108 |
| Nonempty 2-second windows | 81 | 81 |
| Windows using all ten lanes | 40 | 81 |
| Mean active lanes / 2s | 9.395062 | 10.000000 |
| Mean squared-share concentration / 2s | 0.145892 | 0.135732 |
| 8-second hotspots (20 heads, 15pp) | 1 | 0 |

E+ moved 349 notes in 300 original E groups; 331 accepted moves include groups
moved more than once. It used 770,410 safety-note visits, without exhausting
the budget or reverting. The original-source audit passed: 2,076 retained
source notes, 667 E-generated additions, zero drops, retiming, changed hold
durations, collisions, LN conflicts, created repeats or generated LNs outside
one original LN span.

An independent parser of both serialized osu files confirmed that the complete
multiset of every non-lane HitObjects field is identical, including head/tail
times and hit-sample data. Its 8-second lane counts matched both diagnostics.
The new E bytes matched the user's earlier saved E file. A repeat E+ conversion
produced identical chart, conversion report and hotspot sidecar bytes. Source
and saved E hashes remained unchanged. A distinct E+ file was added beside the
original chart for playtesting using an exclusive, non-overwriting copy and
its hash was checked against the validated output.

This is evidence of redistribution and preservation on one chart, not a
general playability improvement. Actual playing, hand ergonomics, perceived
difficulty, very large real collections and cross-platform bit identity remain
unverified. BMS diagnostics describe the normalized pre-export population;
existing BMS timing quantization is outside this post-pass's contract.
