#!/usr/bin/env python3
"""Train a tiny additive Transformer lane policy for KeyWeaver experiments.

This learns three related targets from native 10K references compressed to 7K:

- primary lane placement for the compressed anchor note
- whether companion notes should be added
- how many companion notes and which lanes they should occupy

The output is an experimental local artifact for inspection, not a release path.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import torch
from torch import nn

from train_transformer_lane_policy import (
    DEFAULT_RAY,
    ReferenceChart,
    collect_references,
    default_songs_root,
    direct_lane_set,
    downproject_lane,
    inserted_lane_set,
    load_chart_text,
    parse_meta,
    reference_matches_author,
    split_reference_paths,
    token_list,
)
from transformer_ray_smoke import (
    Note,
    build_features,
    choose_lanes,
    count_final_conflicts,
    direct_lane,
    export_osu,
    parse_int,
    parse_notes,
    read_key,
    repair_created_jacks,
    unique_path,
)


DEFAULT_VARIANTS = (
    {"name": "learned_balanced", "countBias": 0.0, "maxChord": 8},
    {"name": "learned_medium", "countBias": 0.7, "maxChord": 9},
    {"name": "learned_max", "countBias": 1.2, "maxChord": 10},
)


@dataclass
class AdditiveChunk:
    features: torch.Tensor
    primary_labels: torch.Tensor
    add_labels: torch.Tensor
    count_labels: torch.Tensor
    source_path: str
    source_keys: int


class TinyAdditiveTransformerPolicy(nn.Module):
    def __init__(self, target_keys: int, feature_count: int = 12, d_model: int = 48, max_add_count: int = 3) -> None:
        super().__init__()
        self.target_keys = target_keys
        self.max_add_count = max_add_count
        self.input = nn.Linear(feature_count, d_model)
        layer = nn.TransformerEncoderLayer(
            d_model=d_model,
            nhead=4,
            dim_feedforward=96,
            dropout=0.0,
            batch_first=True,
            activation="gelu",
        )
        self.encoder = nn.TransformerEncoder(layer, num_layers=2)
        self.primary_head = nn.Linear(d_model, target_keys)
        self.add_head = nn.Linear(d_model, target_keys)
        self.count_head = nn.Linear(d_model, max_add_count + 1)

    def forward(self, features: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        if features.numel() == 0:
            empty = torch.zeros((0, self.target_keys), dtype=features.dtype, device=features.device)
            empty_count = torch.zeros((0, self.max_add_count + 1), dtype=features.dtype, device=features.device)
            return empty, empty, empty_count
        x = self.input(features)
        positions = torch.arange(features.shape[0], dtype=features.dtype, device=features.device).unsqueeze(1)
        div = torch.exp(
            torch.arange(0, x.shape[-1], 2, dtype=features.dtype, device=features.device)
            * (-torch.log(torch.tensor(10000.0, dtype=features.dtype, device=features.device)) / x.shape[-1])
        )
        pos = torch.zeros_like(x)
        pos[:, 0::2] = torch.sin(positions * div)
        pos[:, 1::2] = torch.cos(positions * div[: pos[:, 1::2].shape[1]])
        encoded = self.encoder((x + pos).unsqueeze(0)).squeeze(0)
        return self.primary_head(encoded), self.add_head(encoded), self.count_head(encoded)


def resolve_device(name: str) -> torch.device:
    requested = name.casefold()
    if requested == "auto":
        if torch.cuda.is_available():
            return torch.device("cuda")
        try:
            import torch_directml  # type: ignore[import-not-found]

            return torch_directml.device()
        except Exception:
            return torch.device("cpu")
    if requested == "cuda":
        if not torch.cuda.is_available():
            raise SystemExit("CUDA was requested but torch.cuda.is_available() is false")
        return torch.device("cuda")
    if requested == "dml":
        try:
            import torch_directml  # type: ignore[import-not-found]

            return torch_directml.device()
        except Exception as exc:
            raise SystemExit(f"DirectML was requested but torch_directml is unavailable: {exc}") from exc
    if requested == "cpu":
        return torch.device("cpu")
    raise SystemExit(f"Unsupported device: {name}")


def device_description(device: torch.device) -> dict[str, object]:
    description: dict[str, object] = {"type": device.type, "name": str(device)}
    if device.type == "cuda":
        index = device.index if device.index is not None else torch.cuda.current_device()
        description["index"] = int(index)
        description["cudaName"] = torch.cuda.get_device_name(index)
    return description


def model_device(model: nn.Module) -> torch.device:
    return next(model.parameters()).device


def cpu_state_dict(model: nn.Module) -> dict[str, torch.Tensor]:
    return {key: value.detach().cpu().clone() for key, value in model.state_dict().items()}


def group_native_notes(native_notes: Iterable[Note], source_keys: int, target_keys: int) -> list[tuple[int, int, list[Note]]]:
    groups: dict[tuple[int, int], list[Note]] = {}
    for native in native_notes:
        source_lane = downproject_lane(native.source_lane, target_keys, source_keys)
        groups.setdefault((native.time, source_lane), []).append(native)
    grouped = [(time, source_lane, notes) for (time, source_lane), notes in groups.items()]
    grouped.sort(key=lambda item: (item[0], item[1], min(note.source_lane for note in item[2])))
    return grouped


def additive_pair_from_reference(
    path: Path,
    source_keys: int,
    target_keys: int,
    max_notes_per_chart: int,
    max_seq: int,
    max_add_count: int,
) -> tuple[ReferenceChart, list[AdditiveChunk], dict[str, int]]:
    _, _, section_map = load_chart_text(path)
    native_notes = parse_notes(section_map.get("HitObjects", []), target_keys)
    native_notes = native_notes[:max_notes_per_chart]
    anchor_notes: list[Note] = []
    primary_labels: list[int] = []
    add_labels: list[list[float]] = []
    count_labels: list[int] = []
    additive_groups = 0
    hidden_notes = 0

    for index, (time, source_lane, group) in enumerate(group_native_notes(native_notes, source_keys, target_keys)):
        direct = direct_lane(source_lane, source_keys, target_keys)
        unique_lanes = sorted({note.source_lane for note in group}, key=lambda lane: (abs(lane - direct), lane))
        if not unique_lanes:
            continue
        primary_lane = unique_lanes[0]
        additions = unique_lanes[1:]
        primary_native = min(
            group,
            key=lambda note: (0 if note.source_lane == primary_lane else 1, abs(note.source_lane - direct), note.source_lane),
        )
        anchor_notes.append(
            Note(
                index=index,
                time=time,
                source_lane=source_lane,
                lane=direct,
                type_value=primary_native.type_value,
                hold=primary_native.hold,
                end_time=primary_native.end_time,
            )
        )
        primary_labels.append(primary_lane)
        row = [0.0 for _ in range(target_keys)]
        for lane in additions[:max_add_count]:
            row[lane] = 1.0
        add_labels.append(row)
        add_count = min(len(additions), max_add_count)
        count_labels.append(add_count)
        if add_count:
            additive_groups += 1
            hidden_notes += add_count

    features = build_features(anchor_notes, source_keys, target_keys)
    primary_tensor = torch.tensor(primary_labels, dtype=torch.long)
    add_tensor = torch.tensor(add_labels, dtype=torch.float32)
    count_tensor = torch.tensor(count_labels, dtype=torch.long)
    chunks: list[AdditiveChunk] = []
    for start in range(0, len(anchor_notes), max_seq):
        end = min(len(anchor_notes), start + max_seq)
        if end > start:
            chunks.append(
                AdditiveChunk(
                    features[start:end],
                    primary_tensor[start:end],
                    add_tensor[start:end],
                    count_tensor[start:end],
                    str(path),
                    source_keys,
                )
            )

    meta = parse_meta(path)
    reference = ReferenceChart(
        path=str(path),
        title=meta.get("Title", ""),
        version=meta.get("Version", ""),
        creator=meta.get("Creator", ""),
        notes=len(native_notes),
    )
    stats = {
        "nativeNotes": len(native_notes),
        "anchors": len(anchor_notes),
        "additiveGroups": additive_groups,
        "hiddenNotes": hidden_notes,
    }
    return reference, chunks, stats


def evaluate_model(
    model: TinyAdditiveTransformerPolicy,
    chunks: list[AdditiveChunk],
    source_keys: int,
    target_keys: int,
) -> dict[str, object]:
    model_was_training = model.training
    model.eval()
    primary_loss_fn = nn.CrossEntropyLoss()
    count_loss_fn = nn.CrossEntropyLoss()
    add_loss_fn = nn.BCEWithLogitsLoss()
    total_primary_loss = 0.0
    total_add_loss = 0.0
    total_count_loss = 0.0
    total_anchors = 0
    primary_correct = 0
    direct_correct = 0
    count_correct = 0
    add_true_positive = 0
    add_predicted = 0
    add_expected = 0
    add_any_correct = 0
    inserted_expected = 0
    inserted_true_positive = 0
    inserted_predicted = 0
    anchors_by_source_keys: dict[int, int] = {}
    add_expected_by_source_keys: dict[int, int] = {}
    add_predicted_by_source_keys: dict[int, int] = {}
    add_true_positive_by_source_keys: dict[int, int] = {}
    device = model_device(model)
    with torch.no_grad():
        for chunk in chunks:
            features = chunk.features.to(device)
            primary_labels = chunk.primary_labels.to(device)
            add_labels = chunk.add_labels.to(device)
            count_labels = chunk.count_labels.to(device)
            primary_logits, add_logits, count_logits = model(features)
            anchors = int(primary_labels.numel())
            if anchors == 0:
                continue
            primary_loss = primary_loss_fn(primary_logits, primary_labels)
            add_loss = add_loss_fn(add_logits, add_labels)
            count_loss = count_loss_fn(count_logits, count_labels)
            primary_pred = primary_logits.argmax(dim=-1)
            count_pred = count_logits.argmax(dim=-1)
            direct = torch.clamp(
                torch.round(features[:, 1] * max(1, target_keys - 1)).long(),
                0,
                target_keys - 1,
            )
            predicted_add = torch.zeros_like(add_labels)
            for index, count in enumerate(count_pred.tolist()):
                if count <= 0:
                    continue
                top = add_logits[index].topk(min(count, target_keys), dim=-1).indices
                predicted_add[index, top] = 1.0
            expected_add = add_labels > 0.5
            predicted_add_bool = predicted_add > 0.5
            primary_correct += int((primary_pred == primary_labels).sum().item())
            direct_correct += int((direct == primary_labels).sum().item())
            count_correct += int((count_pred == count_labels).sum().item())
            add_true_positive += int((predicted_add_bool & expected_add).sum().item())
            add_predicted += int(predicted_add_bool.sum().item())
            add_expected += int(expected_add.sum().item())
            add_any_correct += int(((predicted_add_bool.sum(dim=-1) > 0) == (expected_add.sum(dim=-1) > 0)).sum().item())
            source_key = chunk.source_keys
            anchors_by_source_keys[source_key] = anchors_by_source_keys.get(source_key, 0) + anchors
            add_expected_by_source_keys[source_key] = add_expected_by_source_keys.get(source_key, 0) + int(expected_add.sum().item())
            add_predicted_by_source_keys[source_key] = add_predicted_by_source_keys.get(source_key, 0) + int(predicted_add_bool.sum().item())
            add_true_positive_by_source_keys[source_key] = add_true_positive_by_source_keys.get(source_key, 0) + int(
                (predicted_add_bool & expected_add).sum().item()
            )
            direct_lanes = direct_lane_set(source_key, target_keys)
            if direct_lanes:
                inserted_mask = torch.tensor(
                    [lane not in direct_lanes for lane in range(target_keys)],
                    dtype=torch.bool,
                    device=device,
                )
                inserted_expected += int((expected_add & inserted_mask.unsqueeze(0)).sum().item())
                inserted_predicted += int((predicted_add_bool & inserted_mask.unsqueeze(0)).sum().item())
                inserted_true_positive += int((predicted_add_bool & expected_add & inserted_mask.unsqueeze(0)).sum().item())
            total_primary_loss += float(primary_loss.item()) * anchors
            total_add_loss += float(add_loss.item()) * anchors
            total_count_loss += float(count_loss.item()) * anchors
            total_anchors += anchors
    if model_was_training:
        model.train()
    return {
        "primaryLoss": total_primary_loss / max(1, total_anchors),
        "addLoss": total_add_loss / max(1, total_anchors),
        "countLoss": total_count_loss / max(1, total_anchors),
        "primaryAccuracy": primary_correct / max(1, total_anchors),
        "directPrimaryAccuracy": direct_correct / max(1, total_anchors),
        "countAccuracy": count_correct / max(1, total_anchors),
        "addAnyAccuracy": add_any_correct / max(1, total_anchors),
        "addLabelRate": add_expected / max(1, total_anchors),
        "addPrecision": add_true_positive / max(1, add_predicted),
        "addRecall": add_true_positive / max(1, add_expected),
        "insertedAddPrecision": inserted_true_positive / max(1, inserted_predicted),
        "insertedAddRecall": inserted_true_positive / max(1, inserted_expected),
        "anchors": float(total_anchors),
        "expectedAddedNotes": float(add_expected),
        "predictedAddedNotes": float(add_predicted),
        "anchorsBySourceKeys": {str(key): float(value) for key, value in sorted(anchors_by_source_keys.items())},
        "expectedAddedNotesBySourceKeys": {
            str(key): float(value) for key, value in sorted(add_expected_by_source_keys.items())
        },
        "predictedAddedNotesBySourceKeys": {
            str(key): float(value) for key, value in sorted(add_predicted_by_source_keys.items())
        },
        "addPrecisionBySourceKeys": {
            str(key): add_true_positive_by_source_keys.get(key, 0) / max(1, add_predicted_by_source_keys.get(key, 0))
            for key in sorted(anchors_by_source_keys)
        },
        "addRecallBySourceKeys": {
            str(key): add_true_positive_by_source_keys.get(key, 0) / max(1, add_expected_by_source_keys.get(key, 0))
            for key in sorted(anchors_by_source_keys)
        },
    }


def train_model(
    model: TinyAdditiveTransformerPolicy,
    train_chunks: list[AdditiveChunk],
    validation_chunks: list[AdditiveChunk],
    source_keys: int,
    target_keys: int,
    epochs: int,
    learning_rate: float,
    add_loss_weight: float,
    count_loss_weight: float,
    positive_add_weight: float,
    count_weight_cap: float,
    checkpoint_epochs: set[int] | None = None,
) -> tuple[list[dict[str, object]], dict[int, dict[str, torch.Tensor]]]:
    device = model_device(model)
    optimizer = torch.optim.AdamW(model.parameters(), lr=learning_rate, weight_decay=1e-4)
    primary_loss_fn = nn.CrossEntropyLoss()
    count_totals = torch.zeros(model.max_add_count + 1, dtype=torch.float32)
    for chunk in train_chunks:
        count_totals += torch.bincount(chunk.count_labels, minlength=model.max_add_count + 1).float()
    count_weights = count_totals.sum() / (max(1, model.max_add_count + 1) * count_totals.clamp_min(1.0))
    count_weights = count_weights.clamp(max=count_weight_cap)
    count_loss_fn = nn.CrossEntropyLoss(weight=count_weights.to(device))
    add_loss_fn = nn.BCEWithLogitsLoss(pos_weight=torch.full((target_keys,), positive_add_weight, device=device))
    history: list[dict[str, object]] = []
    checkpoint_states: dict[int, dict[str, torch.Tensor]] = {}
    checkpoint_epochs = checkpoint_epochs or set()
    for epoch in range(1, epochs + 1):
        model.train()
        total_loss = 0.0
        total_anchors = 0
        primary_correct = 0
        add_true_positive = 0
        add_expected = 0
        add_predicted = 0
        count_correct = 0
        for chunk in train_chunks:
            optimizer.zero_grad(set_to_none=True)
            features = chunk.features.to(device)
            primary_labels = chunk.primary_labels.to(device)
            add_labels = chunk.add_labels.to(device)
            count_labels = chunk.count_labels.to(device)
            primary_logits, add_logits, count_logits = model(features)
            primary_loss = primary_loss_fn(primary_logits, primary_labels)
            add_loss = add_loss_fn(add_logits, add_labels)
            count_loss = count_loss_fn(count_logits, count_labels)
            loss = primary_loss + add_loss_weight * add_loss + count_loss_weight * count_loss
            loss.backward()
            optimizer.step()
            with torch.no_grad():
                primary_pred = primary_logits.argmax(dim=-1)
                count_pred = count_logits.argmax(dim=-1)
                add_pred = torch.sigmoid(add_logits) >= 0.5
                add_expected_bool = add_labels > 0.5
                primary_correct += int((primary_pred == primary_labels).sum().item())
                count_correct += int((count_pred == count_labels).sum().item())
                add_true_positive += int((add_pred & add_expected_bool).sum().item())
                add_expected += int(add_expected_bool.sum().item())
                add_predicted += int(add_pred.sum().item())
            anchors = int(primary_labels.numel())
            total_loss += float(loss.item()) * anchors
            total_anchors += anchors
        entry = {
            "epoch": float(epoch),
            "loss": total_loss / max(1, total_anchors),
            "primaryAccuracy": primary_correct / max(1, total_anchors),
            "countAccuracy": count_correct / max(1, total_anchors),
            "addPrecision": add_true_positive / max(1, add_predicted),
            "addRecall": add_true_positive / max(1, add_expected),
            "anchors": float(total_anchors),
        }
        if validation_chunks:
            validation = evaluate_model(model, validation_chunks, source_keys, target_keys)
            entry.update({"val" + key[0].upper() + key[1:]: value for key, value in validation.items()})
        history.append(entry)
        if epoch in checkpoint_epochs:
            checkpoint_states[epoch] = cpu_state_dict(model)
    return history, checkpoint_states


def note_is_safe(notes: list[Note], candidate: Note, jack_window_ms: int) -> bool:
    for note in notes:
        if note.lane != candidate.lane:
            continue
        if note.time == candidate.time:
            return False
        if note.hold and note.end_time is not None and note.time < candidate.time <= note.end_time:
            return False
        if abs(note.time - candidate.time) <= jack_window_ms and note.source_lane != candidate.source_lane:
            return False
    return True


def add_learned_notes(
    notes: list[Note],
    add_logits: torch.Tensor,
    count_logits: torch.Tensor,
    source_keys: int,
    target_keys: int,
    count_bias: float,
    max_chord: int,
    jack_window_ms: int,
) -> dict[str, object]:
    chord_sizes: dict[int, int] = {}
    for note in notes:
        chord_sizes[note.time] = chord_sizes.get(note.time, 0) + 1
    inserted_lanes = inserted_lane_set(source_keys, target_keys)
    generated: list[Note] = []
    skipped_unsafe = 0
    skipped_chord_full = 0
    inserted_generated = 0
    count_bias_tensor = torch.arange(count_logits.shape[-1], dtype=count_logits.dtype, device=count_logits.device) * count_bias
    for index, note in enumerate(notes):
        biased_counts = count_logits[index].detach().float() + count_bias_tensor
        add_count = int(biased_counts.argmax().item())
        if add_count <= 0:
            continue
        if chord_sizes.get(note.time, 0) >= max_chord:
            skipped_chord_full += 1
            continue
        lane_order = add_logits[index].detach().float().argsort(descending=True).tolist()
        added_for_anchor = 0
        for lane in lane_order:
            if added_for_anchor >= add_count:
                break
            if lane == note.lane:
                continue
            if chord_sizes.get(note.time, 0) >= max_chord:
                skipped_chord_full += 1
                break
            candidate = Note(
                index=len(notes) + len(generated),
                time=note.time,
                source_lane=note.source_lane,
                lane=lane,
                type_value=1,
                hold=False,
                end_time=None,
            )
            if not note_is_safe(notes + generated, candidate, jack_window_ms):
                skipped_unsafe += 1
                continue
            generated.append(candidate)
            chord_sizes[note.time] = chord_sizes.get(note.time, 0) + 1
            if lane in inserted_lanes:
                inserted_generated += 1
            added_for_anchor += 1
    notes.extend(generated)
    notes.sort(key=lambda note: (note.time, note.lane, note.index))
    lane_distribution = [0 for _ in range(target_keys)]
    for note in notes:
        if 0 <= note.lane < target_keys:
            lane_distribution[note.lane] += 1
    return {
        "generatedNotes": len(generated),
        "insertedGeneratedNotes": inserted_generated,
        "skippedUnsafe": skipped_unsafe,
        "skippedChordFull": skipped_chord_full,
        "finalNoteCount": len(notes),
        "laneDistributionAfterAdd": lane_distribution,
    }


def apply_model_to_ray(
    model: TinyAdditiveTransformerPolicy,
    ray_path: Path,
    output_dir: Path,
    source_keys: int,
    target_keys: int,
    transformer_weight: float,
    direct_score_weight: float,
    coverage_weight: float,
    inserted_lane_bonus: float,
    count_bias: float,
    max_chord: int,
    jack_window_ms: int,
    name: str,
) -> dict[str, object]:
    preamble, sections, section_map = load_chart_text(ray_path)
    source_keys = source_keys or parse_int(read_key(section_map.get("Difficulty", []), "CircleSize"), 7)
    notes = parse_notes(section_map.get("HitObjects", []), source_keys)
    original_notes = len(notes)
    device = model_device(model)
    features = build_features(notes, source_keys, target_keys).to(device)
    with torch.no_grad():
        primary_logits, add_logits, count_logits = model(features)
    primary_logits = primary_logits.detach().cpu()
    add_logits = add_logits.detach().cpu()
    count_logits = count_logits.detach().cpu()
    policy_stats = choose_lanes(
        notes,
        primary_logits,
        source_keys,
        target_keys,
        transformer_weight,
        direct_score_weight=direct_score_weight,
        coverage_weight=coverage_weight,
        inserted_lane_bonus=inserted_lane_bonus,
    )
    add_stats = add_learned_notes(
        notes,
        add_logits,
        count_logits,
        source_keys,
        target_keys,
        count_bias,
        max_chord,
        jack_window_ms,
    )
    repair_stats = repair_created_jacks(notes, target_keys)
    final_stats = count_final_conflicts(notes)
    output_path = unique_path(output_dir / f"ray_additive_{name}.osu")
    output_text = export_osu(
        preamble,
        sections,
        notes,
        target_keys,
        f"KeyWeaverLearnedAdditive-{name}",
    )
    output_path.write_text(output_text, encoding="utf-8", newline="\n")
    return {
        "name": name,
        "input": str(ray_path),
        "output": str(output_path),
        "sourceKeys": source_keys,
        "targetKeys": target_keys,
        "originalNotes": original_notes,
        "notes": len(notes),
        "directLanes": sorted(direct_lane_set(source_keys, target_keys)),
        "insertedLanes": sorted(inserted_lane_set(source_keys, target_keys)),
        "policy": policy_stats,
        "add": add_stats,
        "repair": repair_stats,
        "finalSafety": final_stats,
        "knobs": {
            "transformerWeight": transformer_weight,
            "directScoreWeight": direct_score_weight,
            "coverageWeight": coverage_weight,
            "insertedLaneBonus": inserted_lane_bonus,
            "countBias": count_bias,
            "maxChord": max_chord,
        },
    }


def export_ray_variants(
    model: TinyAdditiveTransformerPolicy,
    ray_input: Path,
    output_dir: Path,
    source_keys: int,
    target_keys: int,
    jack_window_ms: int,
) -> list[dict[str, object]]:
    return [
        apply_model_to_ray(
            model,
            ray_input,
            output_dir,
            source_keys,
            target_keys,
            0.95,
            0.50,
            0.95,
            0.30,
            0.0,
            8,
            jack_window_ms,
            "balanced",
        ),
        apply_model_to_ray(
            model,
            ray_input,
            output_dir,
            source_keys,
            target_keys,
            1.10,
            0.40,
            1.05,
            0.42,
            0.5,
            9,
            jack_window_ms,
            "medium",
        ),
        apply_model_to_ray(
            model,
            ray_input,
            output_dir,
            source_keys,
            target_keys,
            1.25,
            0.30,
            1.15,
            0.55,
            1.0,
            10,
            jack_window_ms,
            "max",
        ),
    ]


def build_dataset(
    references: list[Path],
    source_keys_list: list[int],
    target_keys: int,
    max_notes_per_chart: int,
    max_seq: int,
    max_add_count: int,
    excluded_authors_by_source_keys: dict[int, set[str]] | None = None,
) -> tuple[list[ReferenceChart], list[AdditiveChunk], dict[str, int], dict[str, dict[str, int]]]:
    manifest: list[ReferenceChart] = []
    chunks: list[AdditiveChunk] = []
    totals = {"nativeNotes": 0, "anchors": 0, "additiveGroups": 0, "hiddenNotes": 0}
    totals_by_source_keys = {
        str(source_keys): {"nativeNotes": 0, "anchors": 0, "additiveGroups": 0, "hiddenNotes": 0, "charts": 0, "skippedCharts": 0}
        for source_keys in source_keys_list
    }
    excluded_authors_by_source_keys = excluded_authors_by_source_keys or {}
    for source_keys in source_keys_list:
        source_key = str(source_keys)
        excluded_authors = excluded_authors_by_source_keys.get(source_keys, set())
        for path in references:
            if excluded_authors:
                meta = parse_meta(path)
                if any(reference_matches_author(path, meta, author) for author in excluded_authors):
                    totals_by_source_keys[source_key]["skippedCharts"] += 1
                    continue
            reference, chart_chunks, stats = additive_pair_from_reference(
                path,
                source_keys,
                target_keys,
                max_notes_per_chart,
                max_seq,
                max_add_count,
            )
            if chart_chunks:
                manifest.append(reference)
                chunks.extend(chart_chunks)
                totals_by_source_keys[source_key]["charts"] += 1
                for key in totals:
                    totals[key] += stats[key]
                    totals_by_source_keys[source_key][key] += stats[key]
    return manifest, chunks, totals, totals_by_source_keys


def parse_checkpoint_epochs(value: str, max_epoch: int) -> list[int]:
    if not value.strip():
        return []
    epochs: set[int] = set()
    for raw_part in value.split(","):
        part = raw_part.strip()
        if not part:
            continue
        try:
            epoch = int(part)
        except ValueError as exc:
            raise SystemExit(f"Invalid checkpoint epoch: {part}") from exc
        if epoch < 1 or epoch > max_epoch:
            raise SystemExit(f"Checkpoint epoch {epoch} is outside 1..{max_epoch}")
        epochs.add(epoch)
    return sorted(epochs)


def parse_source_keys_list(value: str, fallback: int, target_keys: int) -> list[int]:
    raw_values = value if value.strip() else str(fallback)
    keys: set[int] = set()
    for raw_part in raw_values.split(","):
        part = raw_part.strip()
        if not part:
            continue
        try:
            source_keys = int(part)
        except ValueError as exc:
            raise SystemExit(f"Invalid source key count: {part}") from exc
        if source_keys < 1 or source_keys > target_keys:
            raise SystemExit(f"Source key count {source_keys} is outside 1..{target_keys}")
        keys.add(source_keys)
    return sorted(keys)


def parse_source_author_excludes(values: list[str]) -> dict[int, set[str]]:
    excludes: dict[int, set[str]] = {}
    for value in values:
        for item in value.split(";"):
            item = item.strip()
            if not item:
                continue
            if ":" not in item:
                raise SystemExit(f"Invalid source author exclude rule: {item}")
            raw_key, raw_authors = item.split(":", 1)
            try:
                source_keys = int(raw_key.strip())
            except ValueError as exc:
                raise SystemExit(f"Invalid source key count in exclude rule: {item}") from exc
            authors = {author.strip() for author in raw_authors.split(",") if author.strip()}
            if authors:
                excludes.setdefault(source_keys, set()).update(authors)
    return excludes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--songs-root", default=default_songs_root())
    parser.add_argument("--author", action="append", default=["u_e", "CircusGalop"])
    parser.add_argument("--max-charts", type=int, default=200)
    parser.add_argument("--source-keys", type=int, default=7)
    parser.add_argument("--source-keys-list", default="")
    parser.add_argument("--target-keys", type=int, default=10)
    parser.add_argument("--max-notes-per-chart", type=int, default=900)
    parser.add_argument("--max-seq", type=int, default=256)
    parser.add_argument("--max-add-count", type=int, default=3)
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--checkpoint-epochs", default="")
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--add-loss-weight", type=float, default=2.5)
    parser.add_argument("--count-loss-weight", type=float, default=1.2)
    parser.add_argument("--positive-add-weight", type=float, default=80.0)
    parser.add_argument("--count-weight-cap", type=float, default=24.0)
    parser.add_argument("--val-ratio", type=float, default=0.2)
    parser.add_argument("--min-val-charts", type=int, default=12)
    parser.add_argument("--seed", type=int, default=20260609)
    parser.add_argument("--ray-input", default=DEFAULT_RAY)
    parser.add_argument("--output-dir", default="dist/transformer-additive-policy-200")
    parser.add_argument("--jack-window-ms", type=int, default=500)
    parser.add_argument("--exclude-source-author", action="append", default=[])
    parser.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda", "dml"])
    args = parser.parse_args()

    if not args.songs_root:
        raise SystemExit("Set --songs-root, KEYWEAVER_SONGS_ROOT, or OSU_SONGS_ROOT to a local osu! Songs folder.")

    torch.manual_seed(args.seed)
    device = resolve_device(args.device)
    if device.type == "cpu":
        torch.set_num_threads(1)
    authors = token_list(args.author)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    checkpoint_epochs = parse_checkpoint_epochs(args.checkpoint_epochs, args.epochs)
    training_source_keys = parse_source_keys_list(args.source_keys_list, args.source_keys, args.target_keys)
    excluded_authors_by_source_keys = parse_source_author_excludes(args.exclude_source_author)

    references = collect_references(Path(args.songs_root), authors, args.target_keys, args.max_charts)
    if not references:
        raise SystemExit("No reference charts found")
    train_refs, val_refs = split_reference_paths(references, args.val_ratio, args.min_val_charts, args.seed)
    train_manifest, train_chunks, train_totals, train_totals_by_source_keys = build_dataset(
        train_refs,
        training_source_keys,
        args.target_keys,
        args.max_notes_per_chart,
        args.max_seq,
        args.max_add_count,
        excluded_authors_by_source_keys,
    )
    val_manifest, val_chunks, val_totals, val_totals_by_source_keys = build_dataset(
        val_refs,
        training_source_keys,
        args.target_keys,
        args.max_notes_per_chart,
        args.max_seq,
        args.max_add_count,
        excluded_authors_by_source_keys,
    )
    for item in train_manifest:
        item.split = "train"
    for item in val_manifest:
        item.split = "validation"
    if not train_chunks:
        raise SystemExit("No training chunks built")

    model = TinyAdditiveTransformerPolicy(args.target_keys, max_add_count=args.max_add_count).to(device)
    history, checkpoint_states = train_model(
        model,
        train_chunks,
        val_chunks,
        args.source_keys,
        args.target_keys,
        args.epochs,
        args.learning_rate,
        args.add_loss_weight,
        args.count_loss_weight,
        args.positive_add_weight,
        args.count_weight_cap,
        set(checkpoint_epochs),
    )
    final_validation = evaluate_model(model, val_chunks, args.source_keys, args.target_keys) if val_chunks else {}

    model_path = output_dir / "tiny_additive_transformer_policy.pt"
    torch.save(
        {
            "state_dict": cpu_state_dict(model),
            "source_keys": args.source_keys,
            "source_keys_list": training_source_keys,
            "target_keys": args.target_keys,
            "max_add_count": args.max_add_count,
            "seed": args.seed,
            "authors": authors,
            "excluded_authors_by_source_keys": {
                str(key): sorted(value) for key, value in sorted(excluded_authors_by_source_keys.items())
            },
            "history": history,
            "validation": final_validation,
            "device": device_description(device),
        },
        model_path,
    )
    manifest_path = output_dir / "dataset_manifest.json"
    manifest_path.write_text(
        json.dumps([asdict(item) for item in [*train_manifest, *val_manifest]], indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    ray_input = Path(args.ray_input)
    checkpoint_reports: list[dict[str, object]] = []
    for epoch in checkpoint_epochs:
        state = checkpoint_states.get(epoch)
        if state is None:
            continue
        checkpoint_dir = output_dir / "checkpoints" / f"epoch_{epoch:03d}"
        checkpoint_dir.mkdir(parents=True, exist_ok=True)
        checkpoint_model = TinyAdditiveTransformerPolicy(args.target_keys, max_add_count=args.max_add_count).to(device)
        checkpoint_model.load_state_dict(state)
        checkpoint_model.eval()
        checkpoint_history = [entry for entry in history if int(entry["epoch"]) <= epoch]
        checkpoint_validation = evaluate_model(checkpoint_model, val_chunks, args.source_keys, args.target_keys) if val_chunks else {}
        checkpoint_model_path = checkpoint_dir / "tiny_additive_transformer_policy.pt"
        torch.save(
            {
                "state_dict": cpu_state_dict(checkpoint_model),
                "source_keys": args.source_keys,
                "source_keys_list": training_source_keys,
                "target_keys": args.target_keys,
                "max_add_count": args.max_add_count,
                "seed": args.seed,
                "authors": authors,
                "excluded_authors_by_source_keys": {
                    str(key): sorted(value) for key, value in sorted(excluded_authors_by_source_keys.items())
                },
                "history": checkpoint_history,
                "validation": checkpoint_validation,
                "device": device_description(device),
            },
            checkpoint_model_path,
        )
        checkpoint_ray_variants = export_ray_variants(
            checkpoint_model,
            ray_input,
            checkpoint_dir,
            args.source_keys,
            args.target_keys,
            args.jack_window_ms,
        )
        checkpoint_report = {
            "epoch": epoch,
            "modelPath": str(checkpoint_model_path),
            "validation": checkpoint_validation,
            "rayVariants": checkpoint_ray_variants,
        }
        (checkpoint_dir / "checkpoint_report.json").write_text(
            json.dumps(checkpoint_report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        checkpoint_reports.append(checkpoint_report)

    ray_variants = export_ray_variants(
        model,
        ray_input,
        output_dir,
        args.source_keys,
        args.target_keys,
        args.jack_window_ms,
    )

    report = {
        "modelPath": str(model_path),
        "manifestPath": str(manifest_path),
        "authors": authors,
        "referenceCharts": len(train_manifest) + len(val_manifest),
        "trainReferenceCharts": len(train_manifest),
        "validationReferenceCharts": len(val_manifest),
        "trainingChunks": len(train_chunks),
        "validationChunks": len(val_chunks),
        "sourceKeys": args.source_keys,
        "trainingSourceKeys": training_source_keys,
        "targetKeys": args.target_keys,
        "device": device_description(device),
        "maxNotesPerChart": args.max_notes_per_chart,
        "maxSeq": args.max_seq,
        "maxAddCount": args.max_add_count,
        "epochs": args.epochs,
        "checkpointEpochs": checkpoint_epochs,
        "learningRate": args.learning_rate,
        "addLossWeight": args.add_loss_weight,
        "countLossWeight": args.count_loss_weight,
        "positiveAddWeight": args.positive_add_weight,
        "countWeightCap": args.count_weight_cap,
        "directLanes": sorted(direct_lane_set(args.source_keys, args.target_keys)),
        "insertedLanes": sorted(inserted_lane_set(args.source_keys, args.target_keys)),
        "directLanesBySourceKeys": {
            str(source_keys): sorted(direct_lane_set(source_keys, args.target_keys)) for source_keys in training_source_keys
        },
        "insertedLanesBySourceKeys": {
            str(source_keys): sorted(inserted_lane_set(source_keys, args.target_keys)) for source_keys in training_source_keys
        },
        "excludedAuthorsBySourceKeys": {
            str(key): sorted(value) for key, value in sorted(excluded_authors_by_source_keys.items())
        },
        "trainTotals": train_totals,
        "trainTotalsBySourceKeys": train_totals_by_source_keys,
        "validationTotals": val_totals,
        "validationTotalsBySourceKeys": val_totals_by_source_keys,
        "history": history,
        "validation": final_validation,
        "checkpointReports": checkpoint_reports,
        "rayVariants": ray_variants,
    }
    report_path = output_dir / "additive_train_report.json"
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
