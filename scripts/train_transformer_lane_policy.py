#!/usr/bin/env python3
"""Small supervised lane-policy training smoke for KeyWeaver.

The dataset is built from native 10K reference .osu files by down-projecting
their lanes into a lower source key count and teaching a tiny Transformer to
predict the original 10K lane. This is intentionally a smoke-sized local run,
not a release training pipeline.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import torch
from torch import nn

from transformer_ray_smoke import (
    DEFAULT_RAY,
    Note,
    TinyTransformerPolicy,
    build_features,
    choose_lanes,
    count_final_conflicts,
    direct_lane,
    export_osu,
    parse_int,
    parse_notes,
    read_key,
    repair_created_jacks,
    split_sections,
    unique_path,
)


EXCLUDE_TOKENS = (
    "7to10",
    "7 to 10",
    "7-to-10",
    "4k10c",
    "5k10c",
    "6k10c",
    "4to10c",
    "5to10c",
    "converted",
    "convert",
    "keyweaver",
)

STYLE_EXCLUDE_PATTERNS = (
    r"\d+k\d+c",
    r"\d+to\d+c",
)


@dataclass
class ReferenceChart:
    path: str
    title: str
    version: str
    creator: str
    notes: int
    split: str = "train"


@dataclass
class TrainingChunk:
    features: torch.Tensor
    labels: torch.Tensor
    source_path: str


def lower_text(value: str | None) -> str:
    return (value or "").casefold()


def compact_key(value: str | None) -> str:
    return "".join(ch for ch in lower_text(value) if ch.isalnum())


def token_list(values: Iterable[str]) -> list[str]:
    tokens: list[str] = []
    seen: set[str] = set()
    for value in values:
        for item in value.split(","):
            token = item.strip()
            key = token.casefold()
            if token and key not in seen:
                tokens.append(token)
                seen.add(key)
    return tokens


def has_style_exclude_pattern(text: str) -> bool:
    return any(re.search(pattern, text) for pattern in STYLE_EXCLUDE_PATTERNS)


def iter_osu_paths(root: Path) -> Iterable[Path]:
    rg = shutil.which("rg")
    if rg is not None:
        try:
            completed = subprocess.run(
                [rg, "--files", str(root), "-g", "*.osu"],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                errors="replace",
            )
            for raw in completed.stdout.splitlines():
                yield Path(raw)
            return
        except (OSError, subprocess.CalledProcessError):
            pass

    for directory, _, files in os.walk(root):
        for filename in files:
            if filename.casefold().endswith(".osu"):
                yield Path(directory) / filename


def parse_meta(path: Path) -> dict[str, str]:
    meta: dict[str, str] = {}
    section: str | None = None
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                if section == "HitObjects":
                    break
                continue
            if section in ("General", "Metadata", "Difficulty") and ":" in line:
                key, value = line.split(":", 1)
                meta[key.strip()] = value.strip()
    return meta


def reference_matches_author(path: Path, meta: dict[str, str], author: str) -> bool:
    haystack = "\n".join(
        [
            str(path),
            meta.get("Creator", ""),
            meta.get("Version", ""),
            meta.get("Title", ""),
        ]
    ).casefold()
    return lower_text(author) in haystack


def is_reference_chart(path: Path, meta: dict[str, str], author: str, target_keys: int) -> bool:
    try:
        if int(float(meta.get("Mode", "0"))) != 3:
            return False
        if int(round(float(meta.get("CircleSize", "0")))) != target_keys:
            return False
    except ValueError:
        return False

    haystack = "\n".join(
        [
            str(path),
            meta.get("Creator", ""),
            meta.get("Version", ""),
            meta.get("Title", ""),
        ]
    ).casefold()
    if any(token in haystack for token in EXCLUDE_TOKENS):
        return False
    if has_style_exclude_pattern(compact_key(haystack)):
        return False
    return reference_matches_author(path, meta, author)


def load_chart_text(path: Path) -> tuple[list[str], list[tuple[str, list[str]]], dict[str, list[str]]]:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    preamble, sections = split_sections(text.splitlines())
    return preamble, sections, {name: body for name, body in sections}


def downproject_lane(native_lane: int, native_keys: int, source_keys: int) -> int:
    if native_keys <= 1 or source_keys <= 1:
        return 0
    mapped = float(native_lane) * float(source_keys - 1) / float(native_keys - 1)
    return max(0, min(source_keys - 1, int(round(mapped))))


def direct_lane_set(source_keys: int, target_keys: int) -> set[int]:
    return {direct_lane(lane, source_keys, target_keys) for lane in range(source_keys)}


def inserted_lane_set(source_keys: int, target_keys: int) -> set[int]:
    return set(range(target_keys)) - direct_lane_set(source_keys, target_keys)


def training_pair_from_reference(
    path: Path,
    source_keys: int,
    target_keys: int,
    max_notes_per_chart: int,
    max_seq: int,
) -> tuple[ReferenceChart, list[TrainingChunk]]:
    _, sections, section_map = load_chart_text(path)
    del sections
    native_notes = parse_notes(section_map.get("HitObjects", []), target_keys)
    native_notes = native_notes[:max_notes_per_chart]
    source_notes: list[Note] = []
    labels: list[int] = []
    for native in native_notes:
        source_lane = downproject_lane(native.source_lane, target_keys, source_keys)
        source_notes.append(
            Note(
                index=native.index,
                time=native.time,
                source_lane=source_lane,
                lane=direct_lane(source_lane, source_keys, target_keys),
                type_value=native.type_value,
                hold=native.hold,
                end_time=native.end_time,
            )
        )
        labels.append(native.source_lane)

    features = build_features(source_notes, source_keys, target_keys)
    label_tensor = torch.tensor(labels, dtype=torch.long)
    chunks: list[TrainingChunk] = []
    for start in range(0, len(source_notes), max_seq):
        end = min(len(source_notes), start + max_seq)
        if end > start:
            chunks.append(TrainingChunk(features[start:end], label_tensor[start:end], str(path)))

    meta = parse_meta(path)
    reference = ReferenceChart(
        path=str(path),
        title=meta.get("Title", ""),
        version=meta.get("Version", ""),
        creator=meta.get("Creator", ""),
        notes=len(native_notes),
    )
    return reference, chunks


def collect_references(
    songs_root: Path,
    authors: list[str],
    target_keys: int,
    max_charts: int,
) -> list[Path]:
    if not authors:
        return []
    by_author: dict[str, list[Path]] = {author: [] for author in authors}
    seen: set[str] = set()
    for path in iter_osu_paths(songs_root):
        try:
            meta = parse_meta(path)
        except OSError:
            continue
        for author in authors:
            if len(by_author[author]) >= max_charts:
                continue
            if not is_reference_chart(path, meta, author, target_keys):
                continue
            key = str(path).casefold()
            if key in seen:
                continue
            by_author[author].append(path)
            seen.add(key)
        if all(len(paths) >= max_charts for paths in by_author.values()):
            break

    references: list[Path] = []
    index = 0
    while len(references) < max_charts and any(index < len(by_author[author]) for author in authors):
        for author in authors:
            if index < len(by_author[author]) and len(references) < max_charts:
                references.append(by_author[author][index])
        index += 1
    return references


def split_reference_paths(
    references: list[Path],
    val_ratio: float,
    min_val_charts: int,
    seed: int,
) -> tuple[list[Path], list[Path]]:
    if len(references) <= 1 or val_ratio <= 0.0:
        return references, []
    shuffled = list(references)
    random.Random(seed).shuffle(shuffled)
    requested = max(min_val_charts, int(round(len(shuffled) * val_ratio)))
    val_count = min(len(shuffled) - 1, max(1, requested))
    val_set = {str(path).casefold() for path in shuffled[:val_count]}
    train_paths = [path for path in references if str(path).casefold() not in val_set]
    val_paths = [path for path in references if str(path).casefold() in val_set]
    return train_paths, val_paths


def evaluate_model(
    model: TinyTransformerPolicy,
    chunks: list[TrainingChunk],
    source_keys: int,
    target_keys: int,
    transformer_weight: float,
    direct_score_weight: float,
    inserted_lane_bonus: float,
) -> dict[str, float]:
    loss_fn = nn.CrossEntropyLoss()
    total_loss = 0.0
    total_notes = 0
    correct = 0
    top3_correct = 0
    abs_lane_error = 0.0
    hybrid_correct = 0
    hybrid_top3_correct = 0
    hybrid_abs_lane_error = 0.0
    direct_correct = 0
    direct_abs_lane_error = 0.0
    non_direct_labels = 0
    raw_non_direct_correct = 0
    hybrid_non_direct_correct = 0
    inserted_labels = 0
    raw_inserted_correct = 0
    hybrid_inserted_correct = 0
    raw_inserted_predictions = 0
    hybrid_inserted_predictions = 0
    raw_inserted_true_positive = 0
    hybrid_inserted_true_positive = 0
    inserted_lanes = sorted(inserted_lane_set(source_keys, target_keys))
    was_training = model.training
    device = next(model.parameters()).device
    model.eval()
    with torch.no_grad():
        for chunk in chunks:
            features = chunk.features.to(device)
            labels = chunk.labels.to(device)
            logits = model(features)
            note_count = int(labels.numel())
            if note_count == 0:
                continue
            loss = loss_fn(logits, labels)
            prediction = logits.argmax(dim=-1)
            k = min(3, target_keys)
            topk = logits.topk(k, dim=-1).indices
            direct = torch.clamp(
                torch.round(features[:, 1] * max(1, target_keys - 1)).long(),
                0,
                target_keys - 1,
            )
            normalized = (logits - logits.mean(dim=-1, keepdim=True)) / (
                logits.std(dim=-1, unbiased=False, keepdim=True) + 1e-6
            )
            lanes = torch.arange(target_keys, dtype=features.dtype, device=device)
            direct_score = torch.clamp(
                1.0 - (lanes.unsqueeze(0) - direct.float().unsqueeze(1)).abs() / 4.0,
                min=0.0,
            )
            left_source = features[:, 10].unsqueeze(1) < 0.5
            left_lane = lanes.unsqueeze(0) < (target_keys / 2.0)
            panel_score = torch.where(left_source == left_lane, 0.12, -0.08)
            inserted_score = torch.zeros_like(direct_score)
            if inserted_lanes and inserted_lane_bonus:
                inserted_tensor = torch.tensor(inserted_lanes, dtype=labels.dtype, device=device)
                inserted_mask = (lanes.long().unsqueeze(1) == inserted_tensor.unsqueeze(0)).any(dim=1)
                inserted_score[:, inserted_mask] = inserted_lane_bonus
            hybrid_scores = transformer_weight * normalized + direct_score_weight * direct_score + panel_score + inserted_score
            hybrid_prediction = hybrid_scores.argmax(dim=-1)
            hybrid_topk = hybrid_scores.topk(k, dim=-1).indices
            correct += int((prediction == labels).sum().item())
            top3_correct += int((topk == labels.unsqueeze(-1)).any(dim=-1).sum().item())
            abs_lane_error += float((prediction - labels).abs().sum().item())
            hybrid_correct += int((hybrid_prediction == labels).sum().item())
            hybrid_top3_correct += int((hybrid_topk == labels.unsqueeze(-1)).any(dim=-1).sum().item())
            hybrid_abs_lane_error += float((hybrid_prediction - labels).abs().sum().item())
            direct_correct += int((direct == labels).sum().item())
            direct_abs_lane_error += float((direct - labels).abs().sum().item())
            non_direct_mask = labels != direct
            non_direct_count = int(non_direct_mask.sum().item())
            if non_direct_count:
                non_direct_labels += non_direct_count
                raw_non_direct_correct += int((prediction[non_direct_mask] == labels[non_direct_mask]).sum().item())
                hybrid_non_direct_correct += int(
                    (hybrid_prediction[non_direct_mask] == labels[non_direct_mask]).sum().item()
                )
            if inserted_lanes:
                inserted_tensor = torch.tensor(inserted_lanes, dtype=labels.dtype, device=device)
                label_inserted_mask = (labels.unsqueeze(1) == inserted_tensor.unsqueeze(0)).any(dim=1)
                raw_inserted_mask = (prediction.unsqueeze(1) == inserted_tensor.unsqueeze(0)).any(dim=1)
                hybrid_inserted_mask = (hybrid_prediction.unsqueeze(1) == inserted_tensor.unsqueeze(0)).any(dim=1)
                inserted_count = int(label_inserted_mask.sum().item())
                if inserted_count:
                    inserted_labels += inserted_count
                    raw_inserted_correct += int(
                        (prediction[label_inserted_mask] == labels[label_inserted_mask]).sum().item()
                    )
                    hybrid_inserted_correct += int(
                        (hybrid_prediction[label_inserted_mask] == labels[label_inserted_mask]).sum().item()
                    )
                raw_inserted_predictions += int(raw_inserted_mask.sum().item())
                hybrid_inserted_predictions += int(hybrid_inserted_mask.sum().item())
                raw_inserted_true_positive += int((raw_inserted_mask & label_inserted_mask).sum().item())
                hybrid_inserted_true_positive += int((hybrid_inserted_mask & label_inserted_mask).sum().item())
            total_loss += float(loss.item()) * note_count
            total_notes += note_count
    if was_training:
        model.train()
    return {
        "loss": total_loss / max(1, total_notes),
        "accuracy": correct / max(1, total_notes),
        "top3Accuracy": top3_correct / max(1, total_notes),
        "meanAbsLaneError": abs_lane_error / max(1, total_notes),
        "hybridAccuracy": hybrid_correct / max(1, total_notes),
        "hybridTop3Accuracy": hybrid_top3_correct / max(1, total_notes),
        "hybridMeanAbsLaneError": hybrid_abs_lane_error / max(1, total_notes),
        "directAccuracy": direct_correct / max(1, total_notes),
        "directMeanAbsLaneError": direct_abs_lane_error / max(1, total_notes),
        "nonDirectLabelRate": non_direct_labels / max(1, total_notes),
        "rawNonDirectRecall": raw_non_direct_correct / max(1, non_direct_labels),
        "hybridNonDirectRecall": hybrid_non_direct_correct / max(1, non_direct_labels),
        "insertedLaneLabelRate": inserted_labels / max(1, total_notes),
        "rawInsertedLaneUseRate": raw_inserted_predictions / max(1, total_notes),
        "rawInsertedLanePrecision": raw_inserted_true_positive / max(1, raw_inserted_predictions),
        "rawInsertedLaneRecall": raw_inserted_correct / max(1, inserted_labels),
        "hybridInsertedLaneUseRate": hybrid_inserted_predictions / max(1, total_notes),
        "hybridInsertedLanePrecision": hybrid_inserted_true_positive / max(1, hybrid_inserted_predictions),
        "hybridInsertedLaneRecall": hybrid_inserted_correct / max(1, inserted_labels),
        "notes": float(total_notes),
    }


def train_model(
    model: TinyTransformerPolicy,
    chunks: list[TrainingChunk],
    validation_chunks: list[TrainingChunk],
    epochs: int,
    learning_rate: float,
    source_keys: int,
    target_keys: int,
    transformer_weight: float,
    direct_score_weight: float,
    inserted_lane_bonus: float,
    non_direct_loss_weight: float,
) -> list[dict[str, float]]:
    optimizer = torch.optim.AdamW(model.parameters(), lr=learning_rate, weight_decay=1e-4)
    history: list[dict[str, float]] = []
    loss_fn = nn.CrossEntropyLoss(reduction="none")
    device = next(model.parameters()).device
    for epoch in range(1, epochs + 1):
        model.train()
        total_loss = 0.0
        total_notes = 0
        correct = 0
        for chunk in chunks:
            optimizer.zero_grad(set_to_none=True)
            features = chunk.features.to(device)
            labels = chunk.labels.to(device)
            logits = model(features)
            losses = loss_fn(logits, labels)
            direct = torch.clamp(
                torch.round(features[:, 1] * max(1, target_keys - 1)).long(),
                0,
                target_keys - 1,
            )
            weights = torch.ones_like(losses)
            if non_direct_loss_weight > 1.0:
                weights = torch.where(labels != direct, weights * non_direct_loss_weight, weights)
            loss = (losses * weights).sum() / weights.sum().clamp_min(1.0)
            loss.backward()
            optimizer.step()
            with torch.no_grad():
                prediction = logits.argmax(dim=-1)
                correct += int((prediction == labels).sum().item())
            note_count = int(labels.numel())
            total_loss += float(loss.item()) * note_count
            total_notes += note_count
        entry = {
            "epoch": float(epoch),
            "loss": total_loss / max(1, total_notes),
            "accuracy": correct / max(1, total_notes),
            "notes": float(total_notes),
        }
        if validation_chunks:
            validation = evaluate_model(
                model,
                validation_chunks,
                source_keys,
                target_keys,
                transformer_weight,
                direct_score_weight,
                inserted_lane_bonus,
            )
            entry.update(
                {
                    "valLoss": validation["loss"],
                    "valAccuracy": validation["accuracy"],
                    "valTop3Accuracy": validation["top3Accuracy"],
                    "valMeanAbsLaneError": validation["meanAbsLaneError"],
                    "valHybridAccuracy": validation["hybridAccuracy"],
                    "valHybridTop3Accuracy": validation["hybridTop3Accuracy"],
                    "valHybridMeanAbsLaneError": validation["hybridMeanAbsLaneError"],
                    "valDirectAccuracy": validation["directAccuracy"],
                    "valDirectMeanAbsLaneError": validation["directMeanAbsLaneError"],
                    "valNonDirectLabelRate": validation["nonDirectLabelRate"],
                    "valRawNonDirectRecall": validation["rawNonDirectRecall"],
                    "valHybridNonDirectRecall": validation["hybridNonDirectRecall"],
                    "valInsertedLaneLabelRate": validation["insertedLaneLabelRate"],
                    "valRawInsertedLaneUseRate": validation["rawInsertedLaneUseRate"],
                    "valRawInsertedLanePrecision": validation["rawInsertedLanePrecision"],
                    "valRawInsertedLaneRecall": validation["rawInsertedLaneRecall"],
                    "valHybridInsertedLaneUseRate": validation["hybridInsertedLaneUseRate"],
                    "valHybridInsertedLanePrecision": validation["hybridInsertedLanePrecision"],
                    "valHybridInsertedLaneRecall": validation["hybridInsertedLaneRecall"],
                    "valNotes": validation["notes"],
                }
            )
        history.append(entry)
        progress = (
            f"epoch {epoch}/{epochs} "
            f"loss={entry['loss']:.6f} "
            f"accuracy={entry['accuracy']:.4f}"
        )
        if "valAccuracy" in entry:
            progress += (
                f" valAccuracy={entry['valAccuracy']:.4f}"
                f" valHybridAccuracy={entry['valHybridAccuracy']:.4f}"
            )
        print(progress, file=sys.stderr, flush=True)
    return history


def apply_model_to_ray(
    model: TinyTransformerPolicy,
    ray_path: Path,
    output_dir: Path,
    source_keys: int,
    target_keys: int,
    transformer_weight: float,
    direct_score_weight: float,
    coverage_weight: float,
    inserted_lane_bonus: float,
) -> dict[str, object]:
    preamble, sections, section_map = load_chart_text(ray_path)
    source_keys = source_keys or parse_int(read_key(section_map.get("Difficulty", []), "CircleSize"), 7)
    notes = parse_notes(section_map.get("HitObjects", []), source_keys)
    device = next(model.parameters()).device
    with torch.no_grad():
        logits = model(build_features(notes, source_keys, target_keys).to(device)).cpu()
    policy_stats = choose_lanes(
        notes,
        logits,
        source_keys,
        target_keys,
        transformer_weight,
        direct_score_weight=direct_score_weight,
        coverage_weight=coverage_weight,
        inserted_lane_bonus=inserted_lane_bonus,
    )
    repair_stats = repair_created_jacks(notes, target_keys)
    final_stats = count_final_conflicts(notes)
    output_path = unique_path(output_dir / "ray_trained_transformer_10k.osu")
    output_text = export_osu(
        preamble,
        sections,
        notes,
        target_keys,
        "KeyWeaverTrainedTransformerSmoke-10K",
    )
    output_path.write_text(output_text, encoding="utf-8", newline="\n")
    return {
        "input": str(ray_path),
        "output": str(output_path),
        "sourceKeys": source_keys,
        "targetKeys": target_keys,
        "notes": len(notes),
        "directLanes": sorted(direct_lane_set(source_keys, target_keys)),
        "insertedLanes": sorted(inserted_lane_set(source_keys, target_keys)),
        "policy": policy_stats,
        "repair": repair_stats,
        "finalSafety": final_stats,
    }


def select_device(name: str) -> torch.device:
    normalized = name.strip().casefold()
    if normalized in ("", "auto"):
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    if normalized == "cuda" and not torch.cuda.is_available():
        raise SystemExit("CUDA was requested but torch.cuda.is_available() is false")
    return torch.device(normalized)


def model_config_from_args(args: argparse.Namespace) -> dict[str, int | float]:
    return {
        "feature_count": args.feature_count,
        "d_model": args.d_model,
        "nhead": args.nhead,
        "dim_feedforward": args.dim_feedforward,
        "num_layers": args.num_layers,
        "dropout": args.dropout,
    }


def default_songs_root() -> str:
    return os.environ.get("KEYWEAVER_SONGS_ROOT") or os.environ.get("OSU_SONGS_ROOT") or ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--songs-root", default=default_songs_root())
    parser.add_argument("--author", action="append", default=["u_e", "CircusGalop"])
    parser.add_argument("--max-charts", type=int, default=40)
    parser.add_argument("--source-keys", type=int, default=7)
    parser.add_argument("--target-keys", type=int, default=10)
    parser.add_argument("--max-notes-per-chart", type=int, default=1200)
    parser.add_argument("--max-seq", type=int, default=384)
    parser.add_argument("--epochs", type=int, default=3)
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--non-direct-loss-weight", type=float, default=1.0)
    parser.add_argument("--val-ratio", type=float, default=0.2)
    parser.add_argument("--min-val-charts", type=int, default=4)
    parser.add_argument("--seed", type=int, default=20260609)
    parser.add_argument("--ray-input", default=DEFAULT_RAY)
    parser.add_argument("--transformer-weight", type=float, default=0.45)
    parser.add_argument("--direct-score-weight", type=float, default=1.25)
    parser.add_argument("--coverage-weight", type=float, default=0.55)
    parser.add_argument("--inserted-lane-bonus", type=float, default=0.0)
    parser.add_argument("--feature-count", type=int, default=12)
    parser.add_argument("--d-model", type=int, default=32)
    parser.add_argument("--nhead", type=int, default=4)
    parser.add_argument("--dim-feedforward", type=int, default=64)
    parser.add_argument("--num-layers", type=int, default=2)
    parser.add_argument("--dropout", type=float, default=0.0)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--model-basename", default="tiny_transformer_lane_policy.pt")
    parser.add_argument("--output-dir", default="dist/transformer-train-smoke")
    args = parser.parse_args()

    if not args.songs_root:
        raise SystemExit("Set --songs-root, KEYWEAVER_SONGS_ROOT, or OSU_SONGS_ROOT to a local osu! Songs folder.")

    authors = token_list(args.author)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    device = select_device(args.device)
    model_config = model_config_from_args(args)

    torch.manual_seed(args.seed)
    torch.set_num_threads(1)

    references = collect_references(
        Path(args.songs_root),
        authors,
        args.target_keys,
        args.max_charts,
    )
    if not references:
        raise SystemExit("No reference charts found")

    train_references, validation_references = split_reference_paths(
        references,
        args.val_ratio,
        args.min_val_charts,
        args.seed,
    )

    manifest: list[ReferenceChart] = []
    train_chunks: list[TrainingChunk] = []
    validation_chunks: list[TrainingChunk] = []
    for split, paths, target_chunks in (
        ("train", train_references, train_chunks),
        ("validation", validation_references, validation_chunks),
    ):
        for path in paths:
            reference, chart_chunks = training_pair_from_reference(
                path,
                args.source_keys,
                args.target_keys,
                args.max_notes_per_chart,
                args.max_seq,
            )
            if chart_chunks:
                reference.split = split
                manifest.append(reference)
                target_chunks.extend(chart_chunks)

    if not train_chunks:
        raise SystemExit("No training chunks built from reference charts")

    model = TinyTransformerPolicy(args.target_keys, **model_config).to(device)
    history = train_model(
        model,
        train_chunks,
        validation_chunks,
        args.epochs,
        args.learning_rate,
        args.source_keys,
        args.target_keys,
        args.transformer_weight,
        args.direct_score_weight,
        args.inserted_lane_bonus,
        args.non_direct_loss_weight,
    )
    final_validation = (
        evaluate_model(
            model,
            validation_chunks,
            args.source_keys,
            args.target_keys,
            args.transformer_weight,
            args.direct_score_weight,
            args.inserted_lane_bonus,
        )
        if validation_chunks
        else {}
    )

    model_path = output_dir / args.model_basename
    torch.save(
        {
            "state_dict": {key: value.detach().cpu() for key, value in model.state_dict().items()},
            "source_keys": args.source_keys,
            "target_keys": args.target_keys,
            "seed": args.seed,
            "authors": authors,
            "model_config": model_config,
            "modelConfig": model.config(),
            "device": str(device),
            "val_ratio": args.val_ratio,
            "direct_lanes": sorted(direct_lane_set(args.source_keys, args.target_keys)),
            "inserted_lanes": sorted(inserted_lane_set(args.source_keys, args.target_keys)),
            "non_direct_loss_weight": args.non_direct_loss_weight,
            "transformer_weight": args.transformer_weight,
            "direct_score_weight": args.direct_score_weight,
            "coverage_weight": args.coverage_weight,
            "inserted_lane_bonus": args.inserted_lane_bonus,
            "history": history,
            "validation": final_validation,
        },
        model_path,
    )

    ray_result = apply_model_to_ray(
        model,
        Path(args.ray_input),
        output_dir,
        args.source_keys,
        args.target_keys,
        args.transformer_weight,
        args.direct_score_weight,
        args.coverage_weight,
        args.inserted_lane_bonus,
    )

    manifest_path = output_dir / "dataset_manifest.json"
    manifest_path.write_text(
        json.dumps([asdict(item) for item in manifest], indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    report = {
        "modelPath": str(model_path),
        "manifestPath": str(manifest_path),
        "authors": authors,
        "referenceCharts": len(manifest),
        "trainReferenceCharts": sum(1 for item in manifest if item.split == "train"),
        "validationReferenceCharts": sum(1 for item in manifest if item.split == "validation"),
        "trainingChunks": len(train_chunks),
        "validationChunks": len(validation_chunks),
        "sourceKeys": args.source_keys,
        "targetKeys": args.target_keys,
        "architecture": model.config(),
        "device": str(device),
        "maxNotesPerChart": args.max_notes_per_chart,
        "maxSeq": args.max_seq,
        "epochs": args.epochs,
        "learningRate": args.learning_rate,
        "nonDirectLossWeight": args.non_direct_loss_weight,
        "transformerWeight": args.transformer_weight,
        "directScoreWeight": args.direct_score_weight,
        "coverageWeight": args.coverage_weight,
        "insertedLaneBonus": args.inserted_lane_bonus,
        "valRatio": args.val_ratio,
        "directLanes": sorted(direct_lane_set(args.source_keys, args.target_keys)),
        "insertedLanes": sorted(inserted_lane_set(args.source_keys, args.target_keys)),
        "history": history,
        "validation": final_validation,
        "ray": ray_result,
    }
    report_path = output_dir / "train_report.json"
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
