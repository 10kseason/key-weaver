# Changelog

## Unreleased

- Improved the experimental ONNX lane-policy hook so logits models can fall back through top-ranked safe lane candidates and reports now break down evaluated candidates, fallback relanes, same-lane no-ops, and rejection reasons.
- Added `scripts/measure_transformer_policy.py` to compare rule-based and Transformer/ONNX batch dry-run timing, K-likeness, safety, provider, fallback, and rejection metrics into CSV/JSONL outputs.
- Added `scripts/select_converter_eval_set.py` for reproducible random Songs-folder evaluation-set selection with converted-chart and extreme-chart filters, plus input-list support in the Transformer measurement helper.

## 1.1 - 2026-06-10

- Added an experimental classic-engine batch-only ONNX Runtime lane-policy hook (`--onnx-policy`) with safe fallback, strict mode, report metrics, optional CMake wiring, and `--onnx-provider auto|cpu|cuda|dml` provider selection for GPU-capable ONNX Runtime packages.
- Added batch `--only-source-keys <list>` filtering so large runs can convert only selected source key counts and skip the rest cleanly.
- Documented Transformer/ONNX training data guardrails requiring opt-in chart-only datasets, mapper permission records, sanitized manifests, and no bundled audio/visual beatmap assets.
- Added a GUI Classic fast-batch model preset for a future `Transformer model (u_e X CircusGalop Chart dataset model)` ONNX lane-policy file, and documented the current local eligible corpus counts: 632 u_e/CircusGalop 10K charts plus clean CircusGalop 4K/5K/7K source-key material after converter exclusions.
- Added MSYS2 Ninja-pinned Windows verification helpers for GUI build and GUI smoke checks under `C:\tmp`, avoiding stale locks in OneDrive-backed build folders.
- Added parameterized larger Transformer lane-policy training, ONNX export with sidecar verification metadata, and a manual `scripts/train_large_transformer_model.bat` wrapper that writes the GUI preset model path; release packaging now includes `models` when present.
- Changed GUI Classic fast batch to select the installed u_e/CircusGalop Transformer ONNX lane-policy model by default and run it in strict mode, while keeping single Convert, Matrix, NK2, and direct CLI batch behind their existing rule-based or explicit-flag paths.

## Legacy History

Detailed release notes for 1.0.1 and older are kept in [docs/changelog-legacy.md](docs/changelog-legacy.md). Keep this top-level changelog focused on current release readiness and new changes after 1.1.
