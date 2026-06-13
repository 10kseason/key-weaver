#!/usr/bin/env python3
"""Export a trained KeyWeaver Transformer lane-policy checkpoint to ONNX.

The exported model keeps the C++ runtime contract:

    float32[notes, 12] -> float32[notes, target_keys]

The runtime treats the output as lane logits and still applies deterministic
safety checks before accepting any relane.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import torch

from transformer_ray_smoke import TinyTransformerPolicy


def read_json(path: Path | None) -> dict[str, Any]:
    if path is None or not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def checkpoint_model_config(checkpoint: dict[str, Any]) -> dict[str, int | float]:
    raw = checkpoint.get("model_config") or checkpoint.get("modelConfig") or {}
    return {
        "feature_count": int(raw.get("feature_count", raw.get("featureCount", 12))),
        "d_model": int(raw.get("d_model", raw.get("dModel", 32))),
        "nhead": int(raw.get("nhead", 4)),
        "dim_feedforward": int(raw.get("dim_feedforward", raw.get("dimFeedforward", 64))),
        "num_layers": int(raw.get("num_layers", raw.get("numLayers", 2))),
        "dropout": float(raw.get("dropout", 0.0)),
    }


def load_model(checkpoint_path: Path, target_keys_override: int | None) -> tuple[TinyTransformerPolicy, dict[str, Any]]:
    checkpoint = torch.load(checkpoint_path, map_location="cpu")
    target_keys = int(target_keys_override or checkpoint.get("target_keys") or checkpoint.get("targetKeys") or 10)
    model_config = checkpoint_model_config(checkpoint)
    model = TinyTransformerPolicy(target_keys, **model_config)
    model.load_state_dict(checkpoint["state_dict"])
    model.eval()
    return model, {
        "checkpointPath": str(checkpoint_path),
        "sourceKeys": checkpoint.get("source_keys") or checkpoint.get("sourceKeys"),
        "targetKeys": target_keys,
        "authors": checkpoint.get("authors", []),
        "seed": checkpoint.get("seed"),
        "architecture": model.config(),
        "trainingValidation": checkpoint.get("validation", {}),
    }


def parse_int_list(text: str) -> list[int]:
    values = [int(item.strip()) for item in text.split(",") if item.strip()]
    return values or [16]


def verify_onnx(
    onnx_path: Path,
    model: TinyTransformerPolicy,
    feature_count: int,
    note_counts: list[int],
) -> dict[str, Any]:
    import onnx
    import onnxruntime as ort

    onnx_model = onnx.load(str(onnx_path))
    onnx.checker.check_model(onnx_model)

    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name
    cases: list[dict[str, Any]] = []
    for note_count in note_counts:
        features = torch.randn(max(1, note_count), feature_count, dtype=torch.float32)
        with torch.no_grad():
            expected = model(features).detach().cpu().numpy()
        actual = session.run([output_name], {input_name: features.numpy()})[0]
        max_abs_diff = float(abs(expected - actual).max()) if expected.size else 0.0
        cases.append(
            {
                "notes": int(features.shape[0]),
                "outputShape": list(actual.shape),
                "maxAbsDiffVsPyTorch": max_abs_diff,
            }
        )
    return {
        "onnxChecker": "ok",
        "onnxRuntimeProviders": ort.get_available_providers(),
        "inputName": input_name,
        "outputName": output_name,
        "cases": cases,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--train-report", default="")
    parser.add_argument("--output", default="models/u_e_circusgalop_chart_dataset_lane_policy.onnx")
    parser.add_argument("--manifest", default="")
    parser.add_argument("--target-keys", type=int, default=0)
    parser.add_argument("--opset", type=int, default=18)
    parser.add_argument("--dummy-notes", type=int, default=16)
    parser.add_argument("--verify-notes", default="4,5,16,33")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--legacy-export", action="store_true")
    parser.add_argument("--external-data", action="store_true")
    args = parser.parse_args()

    checkpoint_path = Path(args.checkpoint)
    output_path = Path(args.output)
    manifest_path = Path(args.manifest) if args.manifest else output_path.with_suffix(".json")
    train_report_path = Path(args.train_report) if args.train_report else None

    model, metadata = load_model(checkpoint_path, args.target_keys or None)
    feature_count = int(model.feature_count)
    dummy = torch.zeros(max(1, args.dummy_notes), feature_count, dtype=torch.float32)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if args.legacy_export:
        torch.onnx.export(
            model,
            dummy,
            str(output_path),
            input_names=["features"],
            output_names=["lane_logits"],
            dynamic_axes={
                "features": {0: "notes"},
                "lane_logits": {0: "notes"},
            },
            opset_version=args.opset,
            dynamo=False,
        )
    else:
        torch.onnx.export(
            model,
            (dummy,),
            str(output_path),
            input_names=["features"],
            output_names=["lane_logits"],
            dynamic_shapes={
                "features": {
                    0: torch.export.Dim("notes", min=1),
                    1: feature_count,
                }
            },
            opset_version=args.opset,
            dynamo=True,
            external_data=args.external_data,
        )

    train_report = read_json(train_report_path)
    manifest: dict[str, Any] = {
        "modelName": "Transformer model (u_e X CircusGalop Chart dataset model)",
        "format": "ONNX",
        "runtimeContract": {
            "input": "float32[notes, 12]",
            "output": "float32[notes, targetKeys] lane logits",
            "policy": "advisory; KeyWeaver safety checks may reject predicted relanes",
        },
        "onnxPath": str(output_path),
        "opset": args.opset,
        "exporter": "legacy" if args.legacy_export else "dynamo",
        "externalData": args.external_data,
        "sourceCheckpoint": metadata,
        "sourceTrainReport": str(train_report_path) if train_report_path else "",
        "trainingSummary": {
            "referenceCharts": train_report.get("referenceCharts"),
            "trainReferenceCharts": train_report.get("trainReferenceCharts"),
            "validationReferenceCharts": train_report.get("validationReferenceCharts"),
            "sourceKeys": train_report.get("sourceKeys", metadata.get("sourceKeys")),
            "targetKeys": train_report.get("targetKeys", metadata.get("targetKeys")),
            "maxNotesPerChart": train_report.get("maxNotesPerChart"),
            "maxSeq": train_report.get("maxSeq"),
            "epochs": train_report.get("epochs"),
            "validation": train_report.get("validation", metadata.get("trainingValidation", {})),
            "ray": train_report.get("ray", {}),
        },
    }
    if args.verify:
        manifest["verification"] = verify_onnx(output_path, model, feature_count, parse_int_list(args.verify_notes))

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
