# Lane Policy Student Model

The current ONNX lane policy is a Transformer teacher. It is useful for quality experiments, but large batch inference is expensive because attention scales with the number of notes in a chunk.

The lightweight path is a drop-in MLP student:

- input: `features` as `float32[notes, 12]`
- output: `lane_logits` as `float32[notes, 10]`
- no attention, no dynamic sequence operators, and no state
- advisory-only output, still guarded by KeyWeaver's collision, LN, distance, and created-jack checks

This keeps the runtime contract compatible with `--onnx-policy` while making the model friendlier to CPU, CUDA, DirectML/NPU-style providers, and very large chart batches.

## Self-Test

```powershell
python scripts\train_lane_policy_student.py --self-test --epochs 1 --out build\lane_policy_student_selftest.onnx
```

The self-test trains on synthetic direct-lane labels, exports ONNX, runs `onnx.checker` when available, and verifies the output shape with ONNX Runtime.

## Distill From Teacher

Use a bounded input list first. Teacher inference can still be slow because the teacher is the expensive Transformer.
Charts with known conversion markers are skipped by default; pass `--include-converted-markers` only for an explicit ablation.

```powershell
python scripts\train_lane_policy_student.py `
  --input-list build\songs_7k_input_list.txt `
  --author-token u_e `
  --author-token CircusGalop `
  --teacher-onnx dist\release\KeyWeaver-v1.1-win64-20260613-091540\models\u_e_circusgalop_chart_dataset_lane_policy.onnx `
  --source-keys 7 `
  --target-keys 10 `
  --max-notes 200000 `
  --epochs 4 `
  --out build\lane_policy_student_mlp.onnx
```

For quick iteration, reduce `--max-notes` and `--epochs`. The exported model can be used directly:

```powershell
C:\keyweaver-cmake\KeyWeaver.exe --input-list build\songs_7k_input_list.txt --source 7 --target 10 --batch --batch-quiet --onnx-policy build\lane_policy_student_mlp.onnx --onnx-provider cuda --dry-run
```

## Why MLP First

The C++ feature extractor already provides compact note-local context: source lane, direct target lane, key counts, chart-relative time, previous and next gaps, chord size, hold duration, hand side, and active holds. A small MLP cannot model long phrase intent as well as a Transformer, but it can cheaply bias safe candidate lanes. Because KeyWeaver treats ONNX output as advisory only, a lower-capacity student is acceptable if it improves throughput without bypassing safety rules.

If the MLP loses too much quality, the next step is a small local TCN/Conv1D student with fixed overlap windows. Avoid full self-attention for the large-batch path.
