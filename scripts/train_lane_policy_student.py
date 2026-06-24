#!/usr/bin/env python3
"""Train/export a lightweight drop-in lane-policy student model.

The exported ONNX contract intentionally matches KeyWeaver's current runtime:
  input  features:    float32[notes, 12]
  output lane_logits: float32[notes, target_keys]
"""

from __future__ import annotations

import argparse
import math
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np


FEATURE_COUNT = 12


@dataclass
class Note:
    time: int
    lane: int
    end_time: int | None = None


@dataclass
class Chart:
    path: Path
    creator: str
    version: str
    source_keys: int
    notes: list[Note]


def import_torch():
    import torch
    import torch.nn as nn
    import torch.nn.functional as F

    return torch, nn, F


def import_onnxruntime():
    import onnxruntime as ort

    return ort


def map_lane_direct(source_lane: int, source_keys: int, target_keys: int) -> int:
    if source_keys <= 1 or target_keys <= 1:
        return 0
    mapped = source_lane * (target_keys - 1) / (source_keys - 1)
    return max(0, min(target_keys - 1, int(math.floor(mapped + 0.5))))


def normalized_lane(lane: int, key_count: int) -> float:
    return float(lane) / float(max(1, key_count - 1))


def lane_from_osu_x(x: int, keys: int) -> int:
    if keys <= 0:
        return 0
    return max(0, min(keys - 1, int(float(x) * float(keys) / 512.0)))


def read_input_list(path: Path) -> list[Path]:
    return [
        Path(line.strip())
        for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
        if line.strip()
    ]


def parse_osu_chart(path: Path, source_keys_override: int | None = None) -> Chart | None:
    mode = None
    circle_size = None
    creator = ""
    version = ""
    in_hitobjects = False
    notes: list[Note] = []

    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            in_hitobjects = line == "[HitObjects]"
            continue
        if not in_hitobjects:
            if line.startswith("Mode:"):
                mode = line.split(":", 1)[1].strip()
            elif line.startswith("CircleSize:"):
                circle_size = line.split(":", 1)[1].strip()
            elif line.startswith("Creator:"):
                creator = line.split(":", 1)[1].strip()
            elif line.startswith("Version:"):
                version = line.split(":", 1)[1].strip()
            continue

        parts = line.split(",")
        if len(parts) < 5:
            continue
        try:
            x = int(float(parts[0]))
            time = int(float(parts[2]))
            hit_type = int(parts[3])
        except ValueError:
            continue

        keys = source_keys_override
        if keys is None:
            if circle_size is None:
                continue
            try:
                keys = int(round(float(circle_size)))
            except ValueError:
                continue
        lane = lane_from_osu_x(x, keys)
        end_time = None
        if hit_type & 128 and len(parts) >= 6:
            try:
                end_time = int(float(parts[5].split(":", 1)[0]))
            except ValueError:
                end_time = None
        notes.append(Note(time=time, lane=lane, end_time=end_time))

    if mode != "3":
        return None
    keys = source_keys_override
    if keys is None:
        if circle_size is None:
            return None
        try:
            keys = int(round(float(circle_size)))
        except ValueError:
            return None
    if keys <= 0 or not notes:
        return None
    return Chart(path=path, creator=creator, version=version, source_keys=keys, notes=notes)


def matches_author_tokens(chart: Chart, author_tokens: list[str]) -> bool:
    if not author_tokens:
        return True
    creator = chart.creator.lower()
    return any(token.lower() in creator for token in author_tokens)


def has_converted_marker(chart: Chart) -> bool:
    text = f"{chart.path}\n{chart.version}".lower()
    markers = (
        "keyweaver",
        "convert",
        "converted",
        "4to7c",
        "4to7",
        "7to10",
        "7k10c",
        "4k7c",
        "a7k",
        "a10k",
    )
    if any(marker in text for marker in markers):
        return True

    import re

    return re.search(r"\b[0-9]+k[0-9]+c\b", text) is not None


def sorted_notes(notes: Iterable[Note]) -> list[Note]:
    return sorted(notes, key=lambda note: (note.time, note.lane))


def active_holds_at(hold_starts: list[int], hold_ends: list[int], time: int) -> int:
    import bisect

    started = bisect.bisect_left(hold_starts, time)
    ended = bisect.bisect_left(hold_ends, time)
    return max(0, started - ended)


def features_for_chart(chart: Chart, target_keys: int) -> np.ndarray:
    notes = sorted_notes(chart.notes)
    if not notes:
        return np.zeros((0, FEATURE_COUNT), dtype=np.float32)

    chord_sizes: dict[int, int] = {}
    hold_starts: list[int] = []
    hold_ends: list[int] = []
    for note in notes:
        chord_sizes[note.time] = chord_sizes.get(note.time, 0) + 1
        if note.end_time is not None:
            hold_starts.append(note.time)
            hold_ends.append(note.end_time)
    hold_starts.sort()
    hold_ends.sort()

    min_time = notes[0].time
    max_time = max(note.end_time if note.end_time is not None else note.time for note in notes)
    duration = max(1, max_time - min_time)

    rows: list[list[float]] = []
    for i, note in enumerate(notes):
        source_lane = note.lane
        previous_gap = 2000 if i == 0 else max(0, note.time - notes[i - 1].time)
        next_gap = 2000 if i + 1 >= len(notes) else max(0, notes[i + 1].time - note.time)
        direct = map_lane_direct(source_lane, chart.source_keys, target_keys)
        hold_duration = 0 if note.end_time is None else max(0, note.end_time - note.time)
        active_holds = active_holds_at(hold_starts, hold_ends, note.time)
        chord_size = chord_sizes.get(note.time, 1)
        rows.append(
            [
                normalized_lane(source_lane, chart.source_keys),
                normalized_lane(direct, target_keys),
                float(chart.source_keys) / 32.0,
                float(target_keys) / 32.0,
                float(note.time - min_time) / float(duration),
                min(1.0, float(previous_gap) / 2000.0),
                min(1.0, float(next_gap) / 2000.0),
                min(1.0, float(chord_size) / float(max(1, chart.source_keys))),
                1.0 if note.end_time is not None else 0.0,
                min(1.0, float(hold_duration) / 4000.0),
                0.0 if float(source_lane) < float(chart.source_keys) / 2.0 else 1.0,
                min(1.0, float(active_holds) / float(max(1, chart.source_keys))),
            ]
        )
    return np.asarray(rows, dtype=np.float32)


def collect_features(args: argparse.Namespace) -> np.ndarray:
    paths = read_input_list(args.input_list)
    if args.max_charts:
        paths = paths[: args.max_charts]

    chunks: list[np.ndarray] = []
    total = 0
    for path in paths:
        chart = parse_osu_chart(path, source_keys_override=args.source_keys)
        if chart is None:
            continue
        if not matches_author_tokens(chart, args.author_token):
            continue
        if has_converted_marker(chart) and not args.include_converted_markers:
            continue
        if args.source_keys is not None and chart.source_keys != args.source_keys:
            continue
        features = features_for_chart(chart, args.target_keys)
        if features.size == 0:
            continue
        if args.max_notes and total + features.shape[0] > args.max_notes:
            features = features[: max(0, args.max_notes - total)]
        if features.size:
            chunks.append(features)
            total += features.shape[0]
        if args.max_notes and total >= args.max_notes:
            break

    if not chunks:
        raise RuntimeError("no usable chart features were collected")
    return np.concatenate(chunks, axis=0)


def make_synthetic_features(count: int, source_keys: int, target_keys: int) -> np.ndarray:
    rng = np.random.default_rng(1234)
    source_lanes = rng.integers(0, source_keys, size=count)
    direct_lanes = np.asarray(
        [map_lane_direct(int(lane), source_keys, target_keys) for lane in source_lanes],
        dtype=np.float32,
    )
    features = np.zeros((count, FEATURE_COUNT), dtype=np.float32)
    features[:, 0] = source_lanes / max(1, source_keys - 1)
    features[:, 1] = direct_lanes / max(1, target_keys - 1)
    features[:, 2] = source_keys / 32.0
    features[:, 3] = target_keys / 32.0
    features[:, 4] = rng.random(count)
    features[:, 5] = rng.random(count)
    features[:, 6] = rng.random(count)
    features[:, 7] = rng.random(count)
    features[:, 8] = rng.integers(0, 2, size=count)
    features[:, 9] = rng.random(count)
    features[:, 10] = (source_lanes >= source_keys / 2).astype(np.float32)
    features[:, 11] = rng.random(count)
    return features


def direct_labels_from_features(features: np.ndarray, target_keys: int) -> np.ndarray:
    lanes = np.floor(features[:, 1] * float(max(1, target_keys - 1)) + 0.5).astype(np.int64)
    return np.clip(lanes, 0, target_keys - 1)


def teacher_logits(args: argparse.Namespace, features: np.ndarray) -> np.ndarray:
    if args.teacher_onnx is None:
        labels = direct_labels_from_features(features, args.target_keys)
        logits = np.full((features.shape[0], args.target_keys), -4.0, dtype=np.float32)
        logits[np.arange(features.shape[0]), labels] = 4.0
        return logits

    ort = import_onnxruntime()
    providers = ["CPUExecutionProvider"]
    if args.teacher_provider == "cuda":
        providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    session = ort.InferenceSession(str(args.teacher_onnx), providers=providers)
    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name
    batch = max(1, args.teacher_batch_notes)
    outputs: list[np.ndarray] = []
    for offset in range(0, features.shape[0], batch):
        chunk = features[offset : offset + batch]
        outputs.append(session.run([output_name], {input_name: chunk})[0].astype(np.float32))
    logits = np.concatenate(outputs, axis=0)
    if logits.shape[1] < args.target_keys:
        raise RuntimeError("teacher output width is smaller than target keys")
    return logits[:, : args.target_keys]


def build_student(nn, hidden_size: int, layers: int, target_keys: int):
    modules = []
    width = FEATURE_COUNT
    for _ in range(layers):
        modules.append(nn.Linear(width, hidden_size))
        modules.append(nn.ReLU())
        width = hidden_size
    modules.append(nn.Linear(width, target_keys))
    return nn.Sequential(*modules)


def train_student(args: argparse.Namespace, features: np.ndarray, logits: np.ndarray):
    torch, nn, F = import_torch()
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)

    if args.device == "auto":
        device_name = "cuda" if torch.cuda.is_available() else "cpu"
    else:
        device_name = args.device
    device = torch.device(device_name)

    model = build_student(nn, args.hidden_size, args.layers, args.target_keys).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)

    x = torch.from_numpy(features).to(device)
    teacher = torch.from_numpy(logits).to(device)
    labels = torch.argmax(teacher, dim=1)
    count = x.shape[0]
    indices = torch.arange(count, device=device)
    batch_size = min(max(1, args.batch_size), count)

    for epoch in range(args.epochs):
        permutation = indices[torch.randperm(count, device=device)]
        total_loss = 0.0
        total_correct = 0
        for start in range(0, count, batch_size):
            batch_index = permutation[start : start + batch_size]
            student = model(x[batch_index])
            hard_loss = F.cross_entropy(student, labels[batch_index])
            temperature = args.temperature
            soft_loss = F.kl_div(
                F.log_softmax(student / temperature, dim=1),
                F.softmax(teacher[batch_index] / temperature, dim=1),
                reduction="batchmean",
            ) * (temperature * temperature)
            loss = hard_loss + args.kl_weight * soft_loss
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            optimizer.step()

            total_loss += float(loss.detach().cpu()) * batch_index.numel()
            total_correct += int((torch.argmax(student, dim=1) == labels[batch_index]).sum().detach().cpu())
        print(
            f"epoch={epoch + 1} loss={total_loss / count:.6f} "
            f"teacher_argmax_acc={total_correct / count:.4f}"
        )
    return model.cpu()


def export_onnx(args: argparse.Namespace, model) -> None:
    torch, _, _ = import_torch()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    model.eval()
    dummy = torch.zeros((1, FEATURE_COUNT), dtype=torch.float32)
    torch.onnx.export(
        model,
        dummy,
        str(args.out),
        input_names=["features"],
        output_names=["lane_logits"],
        dynamic_axes={"features": {0: "notes"}, "lane_logits": {0: "notes"}},
        opset_version=args.opset,
    )

    try:
        import onnx

        onnx_model = onnx.load(str(args.out))
        del onnx_model.metadata_props[:]
        metadata = onnx_model.metadata_props.add()
        metadata.key = "keyweaver_model_kind"
        metadata.value = "lane_policy_student_mlp"
        metadata = onnx_model.metadata_props.add()
        metadata.key = "keyweaver_feature_count"
        metadata.value = str(FEATURE_COUNT)
        metadata = onnx_model.metadata_props.add()
        metadata.key = "keyweaver_target_keys"
        metadata.value = str(args.target_keys)
        onnx.checker.check_model(onnx_model)
        onnx.save(onnx_model, str(args.out))
    except Exception as error:  # pragma: no cover - only a warning path for local tooling gaps.
        print(f"warning: ONNX checker/metadata step skipped: {error}")


def validate_export(args: argparse.Namespace) -> None:
    ort = import_onnxruntime()
    session = ort.InferenceSession(str(args.out), providers=["CPUExecutionProvider"])
    output = session.run(
        None,
        {"features": np.zeros((3, FEATURE_COUNT), dtype=np.float32)},
    )[0]
    if output.shape != (3, args.target_keys):
        raise RuntimeError(f"unexpected ONNX output shape: {output.shape}")
    print(f"validated ONNX output shape: {output.shape}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-list", type=Path, help="UTF-8 newline-delimited .osu chart list.")
    parser.add_argument("--teacher-onnx", type=Path, help="Existing teacher ONNX lane policy.")
    parser.add_argument("--teacher-provider", choices=["cpu", "cuda"], default="cpu")
    parser.add_argument("--teacher-batch-notes", type=int, default=512)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source-keys", type=int, default=7)
    parser.add_argument("--target-keys", type=int, default=10)
    parser.add_argument("--author-token", action="append", default=[], help="Keep charts whose Creator contains this token. Repeatable.")
    parser.add_argument("--include-converted-markers", action="store_true", help="Do not skip charts with known conversion markers.")
    parser.add_argument("--max-charts", type=int, default=0)
    parser.add_argument("--max-notes", type=int, default=200_000)
    parser.add_argument("--hidden-size", type=int, default=64)
    parser.add_argument("--layers", type=int, default=2)
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--temperature", type=float, default=2.0)
    parser.add_argument("--kl-weight", type=float, default=0.3)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto")
    parser.add_argument("--opset", type=int, default=18)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--self-test", action="store_true", help="Train/export on synthetic data.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        features = make_synthetic_features(512, args.source_keys, args.target_keys)
    else:
        if args.input_list is None:
            raise RuntimeError("--input-list is required unless --self-test is used")
        features = collect_features(args)

    logits = teacher_logits(args, features)
    print(f"training rows={features.shape[0]} features={features.shape[1]} target_keys={args.target_keys}")
    model = train_student(args, features, logits)
    export_onnx(args, model)
    validate_export(args)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
