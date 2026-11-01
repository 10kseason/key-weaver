# KeyWeaver v1.4.0

New in v1.4.0: CLI `--nk2-e-fullfield` redistributes
completed E notes across 8K/10K while keeping E rhythm and LN lengths. Existing E
and automatic presets stay unchanged. See [E+ usage and limits](docs/nk2-e-fullfield.md).
E+ v2 also addresses source-lane pinning for every 1K–18K input to 8K/10K, with
short-repeat/LN protection and safe 2-second segmentation of long chains.
Outputs carry `-fullfield-v2` to distinguish previous comparisons.
For more chord taps, select **E+ - Full-field + taps** / `--nk2-e-tap-boost`.
Choose Target **8** or **10**; single conversion, batch and hotspot reports support both.
It keeps completed E+ placements and LNs, adding taps only at retained source
head times. Tap boost v2 uses a half-beat repeat guard clamped to 180–500ms only
for new taps, balances their lanes and reserves budgets across the song. Base
notes/LNs stay fixed; strict 500ms repeats are reported separately.
See [bounds and validation](docs/nk2-e-tap-boost.md).

New in v1.4.0: `--pattern-hotspot-report <new.json>` adds an optional
8-second lane-use diagnostic for final NK2 E 8K/10K results. Conversion and existing
reports remain unchanged. See [usage and limits](docs/pattern-hotspot-audit.md).
Existing 1.3.1 release binaries do not include this option.

Languages: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver is a C++20/CMake key-count converter for osu!mania `.osu` charts and BMS-family charts. It provides local deterministic conversion, a Windows GUI, batch/report output, and an experimental NK2 engine for upward, downward, and same-key transforms through 18K.

## What This Release Is

`v1.4.0` adds E+ Full-field + taps for 8K/10K and optional 8-second hotspot diagnostics. The NK2 GUI now offers E (default), E+ taps, native, transform and harder. Existing CLI recipes and all-key conversion remain available. The release includes a Windows package, full project source and a separate 14-file NK2 source archive.

Main capabilities:

- osu!mania `.osu` input/output.
- Basic BMS/BME/BML/PMS input/output; BMS-family input stays BMS-family output.
- Source key detection with optional Source override.
- GUI Source selector supports `auto` and `1`-`18`; numeric Source in batch skips non-matching source-key charts.
- GUI Target supports 1K through 18K. NK2 CLI/core supports every source/target pair from 1K through 18K, including higher-to-lower conversion.
- Default output beside each input chart when `--out` / `--out-dir` is omitted.
- Parallel CLI batch conversion with `--batch`, `--input-list`, `--jobs`, progress, and summary output.
- Classic conversion with collision/LN/distance/jack safety checks.
- Experimental NK2 conversion modes: `native`, `faithful`, `harder`, `transform`; same-K conversion runs in `transform` or explicit 10K E+, while `report` remains single-input analysis only.
- With `--engine nk2` and no explicit mode/search/note options, detected 7K inputs use B phrase search for 4K/5K and E Source LN for 8K/10K. Other pairs keep the CLI's native fallback. The GUI's NK2 default is now `E - Source LN`, using explicit E flags. CLI auto selection and explicit mode/search/note options retain their behavior. Classic remains the default engine, and the core `Mode` default is unchanged.
- `--nk2-search phrase` compares bounded two-beat placements for faithful conversion between any source and target from 1K through 18K, with stream transforms off. Explicit `--nk2-mode faithful --nk2-search off` selects the legacy faithful path; `--nk2-audit` independently checks its output. See [phrase search and local OSZ comparisons](docs/nk2-phrase-search.md).
- `--nk2-phrase-support` adds conservative support taps to upward or downward phrase output while preserving every retained source placement. Compare B Phrase and C Phrase Notes at 10K with the packager's `--comparison phrase-notes` option.
- Add `--nk2-phrase-dense-ln` to preserve C and append more taps plus long notes. The packager's `--comparison phrase-dense-ln` includes B/C/D 10K and the source 7K for comparison.
- Add `--nk2-phrase-source-ln` alongside the dense option for E: vary generated LN lengths and lanes while keeping each whole LN inside one source LN interval. D remains unchanged. `--comparison phrase-source-ln` packages source 7K and C/D/E 10K.
- Add `--nk2-phrase-source-ln-boost` alongside E for F: preserve all E notes and append only source-covered LNs with higher addition budgets. `--comparison phrase-source-ln-boost` packages source 7K and D/E/F 10K; `--comparison phrase-key-tests` packages source 7K, A/B 5K, and D/E/F 8K. Existing D/E 10K behavior is retained.
- Manual A–F supports all 1K–18K pairs. B adds no notes or timing shifts, including 4K-to-5K; C–F append safe notes without rearranging their base output. Additions can be zero when no safe room remains. `--comparison phrase-four-key-tests` includes source 4K plus B/E at 5K and 7K. `--comparison adopted-repair` includes source 7K, A Fixed 5K, B Adopted 4K/5K, and E Adopted 8K/10K. Existing comparison modes remain available.
- Fixed the legacy source-jack exception to allow only the immediately preceding target lane for that same source lane; continuing a source jack no longer clears the recent-jack mask for every target lane.
- Classic GUI 10K conversions use Full-Field Mirror-Remix by default.
- JSON/CSV reports for conversion quality, policy comparison, and diagnostics.
- Optional batch-only ONNX Runtime lane-policy hook for CUDA builds.

Not included: full chart editing, audio playback, waveform view, DP split conversion, realtime BMS player behavior, training runs, deployment, or automatic publishing.

## Release Package

Use the Windows release zip when you only want to run the tool:

```text
KeyWeaver-v1.4.0-win64-<timestamp>.zip
```

Inside the package:

- `keyconv_gui.exe` - Windows GUI harness.
- `KeyWeaver.exe` - CLI entrypoint. Double-clicking it opens the GUI when `keyconv_gui.exe` is beside it.
- `keyconv.exe` - CLI alias.
- MinGW runtime DLLs.
- `samples/`, `profiles/`, `docs/`, `models/`, `scripts/`.
- `smoke/` verification logs generated by the package script.

## Quick GUI Use

1. Unzip the release package.
2. Run `keyconv_gui.exe`.
3. Select `KeyWeaver.exe` if the GUI did not auto-detect it.
4. Select or drop an osu!mania/BMS-family chart.
5. Set Source to `auto` or a fixed key count from `1` to `18`.
6. Set Target from `1` to `18`.
7. Choose Classic or NK2.
8. Press Convert or Batch Folder.

GUI notes:

- Blank Output means each converted chart is written beside its source chart.
- Batch with numeric Source only converts charts whose detected source key count matches the selected value.
- GUI Matrix is Classic/NK1-only.
- NK2 batch works for conversion modes, but NK2 `report` is single-input only.

NK2 GUI choices:

| Choice | Behavior |
| --- | --- |
| `E - Source LN` | Default NK2 GUI choice; generated LNs stay inside source LN intervals. |
| `E+ - Full-field + taps` | Redistribute E and append beat-spaced chord taps; Target 8K/10K. |
| `native` | Existing native conversion. |
| `transform` | Existing transform conversion, including same-key relaning. |
| `harder` | Existing higher-density native conversion. |

Only these five choices appear in the GUI. Auto, A/B/C/D/F and plain E+ remain
available through their existing CLI recipes for compatibility. CLI automatic
selection still resolves per input. E supports expansion/compression across
1K–18K and skips same-key output; E+ taps explicitly remixes same-key 8K/10K.
Phrase presets require stream transforms off. BMS output remains limited to
its supported layouts.

```powershell
# E expansion: 5K to 12K
build/KeyWeaver.exe input5k.osu --target 12 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln
# F compression: 8K to 6K; safe additions may be zero
build/KeyWeaver.exe input8k.osu --target 6 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --nk2-phrase-source-ln-boost
```

## CLI Examples

Single chart dry run:

```powershell
.\KeyWeaver.exe samples\simple_4k.osu --source 4 --target 10 --dry-run
```

Write a converted chart and report:

```powershell
.\KeyWeaver.exe samples\simple_7k_ln.osu --source 7 --target 10 --out dist\simple_7k_10k.osu --report dist\report.json
```

Fast batch from an input list:

```powershell
.\KeyWeaver.exe --batch --input-list charts.txt --source 7 --target 10 --engine nk2 --nk2-mode native --batch-quiet
```

Mixed-source batch behavior:

```text
--source 7 converts detected 7K charts and skips non-7K charts.
```

BMS rule:

```text
BMS-family input can only write BMS-family output: .bms, .bme, .bml, or .pms.
```

Run `KeyWeaver.exe --help` for the complete option list.

## Build From Source

Default build:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful targeted build:

```powershell
cmake --build build --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
```

Release package:

```powershell
.\scripts\package_release.ps1 -Version 1.4.0
```

The package script runs a Release CMake build, the complete registered CTest suite (including NK2 safety, phrase and CLI regressions), public-header smoke, GUI smoke, CLI dry-run smokes, sample conversions, reconversion guard checks, and BMS guard checks. It writes archives and `.sha256` files under `dist/release/`. Automated checks cover structural safety and deterministic output; in-game play quality still requires manual comparison.

## Optional ONNX CUDA Batch Path

Default builds do not require ONNX Runtime. ONNX support is optional and batch-only.

Configure manually:

```powershell
cmake -S . -B build-onnx -G Ninja -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=<onnxruntime-package-root>
cmake --build build-onnx --target KeyWeaver
```

Windows helper:

```powershell
.\scripts\build_onnx_cuda.bat
```

ONNX notes:

- Current source path supports CUDA provider only.
- The model is advisory. Existing collision, LN, distance, and no-created-jack safety checks still decide accepted lanes.
- Target-10 batch can auto-load `models/lane_policy_student_mlp_u_e_circusgalop.onnx` when bundled.
- Use `--no-auto-onnx-policy` to force deterministic batch behavior.
- Use `--onnx-policy-strict` when validating the runtime setup.

## Source Package

The source package is named like:

```text
KeyWeaver-v1.4.0-source-<timestamp>.zip
```

It contains explicitly selected tracked project files plus the current NK2 source, tests, and documentation. Private audio/training experiments, local MP3/chart/OSZ assets, `AGENTS.md` and other local agent instructions, `.git`, build folders, and old release archives are excluded. It does not include working-tree diff artifacts.

The release also provides `KeyWeaver-NK2-source-1K-18K-v1.4.0-<timestamp>.zip`, containing the 14 files under `src/nk2/`, including the independent safety auditor. This focused archive is for inspection and integration; use the full source archive to build the application.

## Important Docs

- `docs/algorithm-lock-v0.6.0.md` - frozen normal-mode conversion contract.
- `docs/algorithm-lock-v0.6.1.md` - 10K staged planner contract.
- `docs/design-10k-fullfield-remix.md` - GUI 10K Full-Field Mirror-Remix design.
- `docs/nk2-algorithm.md` - current NK2 walkthrough.
- `docs/nk2-phrase-search.md` - adopted/manual phrase presets, independent safety audit, and comparison OSZ packaging.
- `docs/nk2-design.md` - second-generation NK2 design notes.
- `docs/lane-policy-student.md` - ONNX lane-policy student notes.
- `docs/code-architecture.md` - code architecture overview.

## Safety And Limitations

- BMS support is an MVP and does not implement every BMS extension. Safe output layouts are 1K-10K, 12K, 14K, 16K, and 18K; other BMS target counts are rejected instead of silently dropping lanes.
- 9K and 18K BMS-family output defaults to `.pms`; forced 18K parsing preserves all 18 playable channel positions.
- 18K auto detection preserves lane positions when all eighteen channels are used; use `--source 18` for sparse charts with unused lanes.
- NK2 parallel batches preserve same-named inputs in a shared `--out-dir` by assigning unique output suffixes.
- NK2 overflow rolls check both earlier and later placed notes for created jacks; unsafe taps may be dropped even in faithful mode.
- DP conversion is not implemented.
- Beam search is reserved and falls back to greedy behavior.
- Strong key-count compression may drop or roll overflow notes according to the selected compression policy.
- Converted charts are marked and guarded so they are not reconverted accidentally.
- NK2 and ONNX paths are experimental; use the included smoke outputs and reports when comparing behavior.

## License

See `LICENSE`.
