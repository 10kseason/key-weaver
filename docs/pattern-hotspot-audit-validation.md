# NK2 E Hotspot Audit — local verification

Date: 2026-11-01. Base commit: `2c5a035c3f6728f4f94748f90e6d5740fd5351d2`.
This records the initial local validation before publication of the source changes.
No source charts or release artifacts were overwritten.

## Executed checks

Windows, GNU C++ 15.1.0, CMake/Ninja, Release, ONNX Runtime disabled:

```text
cmake -S . -B build-hotspot -G Ninja -DCMAKE_BUILD_TYPE=Release -DKEYWEAVER_WITH_ONNXRUNTIME=OFF
cmake --build build-hotspot -j 4
ctest --test-dir build-hotspot --output-on-failure
git diff --check
```

Build passed. All **8/8 CTest suites passed**: the six existing suites plus
`keyconv_pattern_hotspot_tests` and `keyconv_pattern_hotspot_cli`.
The existing `assignment.cpp` GCC maybe-uninitialized warning also appeared in
the pre-change baseline build. That unrelated source was not modified.

The new C++ suite contains eight groups:

1. Same global distribution, different local concentration; correct flagged lanes.
2. Exactly +15/-15 percentage points, below/above threshold, invalid counts and overflow.
3. Identical JSON after repeated input, reversed/rotated events, and hostile numeric locale;
   all input Chart fields remain unchanged.
4. 19-note windows remain in global totals; 20-note windows are eligible.
5. Half-open boundaries, simultaneous notes, negative/INT_MIN/INT_MAX timestamps,
   and one LN head spanning several windows.
6. Empty/sparse inputs and compressed leading/intermediate empty ranges.
7. Invalid K/lane/type/hold/parser warnings with no partial normal results;
   explicit K preserves unused lanes.
8. Actual E conversion with generated LNs: total/global counts agree with E's
   existing report, full source-LN coverage and independent safety pass, and
   Chart/export/report remain identical before and after analysis.

The CLI suite verifies automatic and explicit E, deterministic JSON, unchanged
converted `.osu` and existing report bytes, dry-run, empty input, parser-warning
status, and pre-export BMS counts with unchanged BMS/report bytes. It rejects
existing destinations, relative/dot/absolute aliases, Windows case/trailing-dot
aliases and alternate streams, other presets/F, Classic, other targets and batch.

A separate local invocation supplied a **pre-change executable** compiled from
the base commit through the CLI test's optional `BASELINE_EXE` parameter.
The synthetic E fixture's exported `.osu` and existing report matched that binary
byte for byte with the new diagnostic enabled. This proves that fixture and the
covered regressions, not every possible input or compiler environment.

## Synthetic distinction oracle

Four 8-second windows contain 20 events each. A has two events in each lane per
window. B has the following lane counts:

```text
8,8,4,0,0,0,0,0,0,0
0,0,4,8,8,0,0,0,0,0
0,0,0,0,0,8,8,4,0,0
0,0,0,0,0,0,0,4,8,8
```

Both total eight events per lane (10%). A flags no windows; B flags four windows,
with lane pairs {0,1}, {3,4}, {5,6}, {8,9} at 40% (+30pp). All 80 events are
accounted for exactly once. The fixture is a diagnostic oracle, not a claim of
playability or compliance with every conversion safety constraint.

## Limits

Real-song playtests, detection/false-positive rates, performance benchmarks,
MSVC/Linux builds, GUI interaction and ONNX builds were not run. The existing
GUI target built, but no new GUI integration was added. BMS diagnostics describe
the final normalized Chart before export quantization, not a reparse guarantee.

No quality improvement is claimed. Suggested evaluation is paired, initially
blinded playback of similarly dense flagged and unflagged E excerpts, including
intentional source repeats and generated LN regions.
