#!/usr/bin/env python3
"""Measure rule-based vs ONNX lane policy behavior through KeyWeaver batch dry-runs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Any


CHART_EXTENSIONS = {".osu", ".bms", ".bme", ".bml", ".pms"}


METRIC_PATTERNS: dict[str, tuple[str, type]] = {
    "notes": (r"^Notes:\s*([0-9]+)", int),
    "sameTimeCollisions": (r"^Same-time collisions:\s*([0-9]+)", int),
    "lnConflicts": (r"^LN conflicts:\s*([0-9]+)", int),
    "createdJacks": (r"^\s*-?\s*Created jacks:\s*([0-9]+)", int),
    "shiftedNotes": (r"^Shifted notes:\s*([0-9]+)", int),
    "kLikenessScore": (r"^\s*-\s*K-likeness score:\s*([0-9.]+)", float),
    "playabilityScore": (r"^\s*-\s*Playability score:\s*([0-9.]+)", float),
    "targetKSafetyScore": (r"^\s*-\s*Target-K safety score:\s*([0-9.]+)", float),
    "onnxAttemptedNotes": (r"^\s*-\s*ONNX policy attempted notes:\s*([0-9]+)", int),
    "onnxEvaluatedCandidates": (r"^\s*-\s*ONNX policy evaluated candidates:\s*([0-9]+)", int),
    "onnxAcceptedRelanes": (r"^\s*-\s*ONNX policy accepted relanes:\s*([0-9]+)", int),
    "onnxFallbackRelanes": (r"^\s*-\s*ONNX policy fallback relanes:\s*([0-9]+)", int),
    "onnxRejectedRelanes": (r"^\s*-\s*ONNX policy rejected relanes:\s*([0-9]+)", int),
    "onnxSameLaneNoops": (r"^\s*-\s*ONNX policy same-lane no-ops:\s*([0-9]+)", int),
    "onnxRejectOutOfRange": (
        r"^\s*-\s*ONNX policy rejected by out-of-range:\s*([0-9]+)",
        int,
    ),
    "onnxRejectCollision": (
        r"^\s*-\s*ONNX policy rejected by collision:\s*([0-9]+)",
        int,
    ),
    "onnxRejectLnConflict": (
        r"^\s*-\s*ONNX policy rejected by LN conflict:\s*([0-9]+)",
        int,
    ),
    "onnxRejectCreatedJack": (
        r"^\s*-\s*ONNX policy rejected by created jack:\s*([0-9]+)",
        int,
    ),
    "cliTotalMs": (r"^Timing:\s*total=([0-9]+)\s*ms", int),
    "batchSucceeded": (r"^Batch summary:\s*succeeded=([0-9]+)", int),
    "batchFailed": (r"^Batch summary:\s*succeeded=[0-9]+\s*failed=([0-9]+)", int),
    "batchSkipped": (
        r"^Batch summary:\s*succeeded=[0-9]+\s*failed=[0-9]+\s*skipped=([0-9]+)",
        int,
    ),
}


BOOL_PATTERNS = {
    "onnxRequested": r"^\s*-\s*ONNX policy requested:\s*(yes|no)",
    "onnxLoaded": r"^\s*-\s*ONNX policy loaded:\s*(yes|no)",
}


TEXT_PATTERNS = {
    "onnxModel": r"^\s*-\s*ONNX policy model:\s*(.+)",
    "onnxRequestedProvider": r"^\s*-\s*ONNX policy (?:requested provider|provider requested):\s*(.+)",
    "onnxActiveProvider": r"^\s*-\s*ONNX policy (?:active provider|provider active):\s*(.+)",
    "onnxAvailableProviders": r"^\s*-\s*ONNX policy available providers:\s*(.+)",
}


SUMMARY_FIELDS = [
    "chart",
    "baselineExit",
    "onnxExit",
    "baselineWallMs",
    "onnxWallMs",
    "wallDeltaMs",
    "baselineCliMs",
    "onnxCliMs",
    "baselineK",
    "onnxK",
    "kDelta",
    "baselineSafety",
    "onnxSafety",
    "baselineCollisions",
    "onnxCollisions",
    "baselineLnConflicts",
    "onnxLnConflicts",
    "baselineCreatedJacks",
    "onnxCreatedJacks",
    "onnxLoaded",
    "onnxProvider",
    "onnxAttempted",
    "onnxEvaluatedCandidates",
    "onnxAccepted",
    "onnxFallback",
    "onnxRejected",
    "onnxSameLaneNoops",
    "onnxRejectCollision",
    "onnxRejectLnConflict",
    "onnxRejectCreatedJack",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare KeyWeaver's deterministic lane policy with the optional "
            "ONNX/Transformer lane policy using batch dry-runs."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="*",
        help=(
            "Chart files or folders. If omitted, samples/simple_4k.osu and "
            "samples/simple_7k_ln.osu are used."
        ),
    )
    parser.add_argument(
        "--input-list",
        action="append",
        default=[],
        help="Text file containing one chart path per line. Repeat for multiple lists.",
    )
    parser.add_argument(
        "--input-base",
        default="",
        help="Base folder used to resolve relative paths from --input-list.",
    )
    parser.add_argument("--exe", default="build/KeyWeaver.exe", help="KeyWeaver executable.")
    parser.add_argument(
        "--model",
        default="models/u_e_circusgalop_chart_dataset_lane_policy.onnx",
        help="ONNX lane-policy model.",
    )
    parser.add_argument("--out-dir", default="dist/transformer-measure", help="Output folder.")
    parser.add_argument("--target", type=int, default=10, help="Target key count.")
    parser.add_argument("--source", type=int, default=None, help="Optional source key count.")
    parser.add_argument(
        "--provider",
        choices=["auto", "cpu", "cuda", "dml"],
        default="cpu",
        help="ONNX Runtime execution provider to request.",
    )
    parser.add_argument(
        "--no-strict",
        action="store_true",
        help="Allow ONNX failures to fall back instead of failing the ONNX run.",
    )
    parser.add_argument("--repeat", type=int, default=1, help="Runs per chart and mode.")
    parser.add_argument("--warmup", type=int, default=0, help="Unrecorded warmup runs per mode.")
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Maximum chart count after discovery. 0 means no limit.",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Additional argument passed through to KeyWeaver. Repeat for multiple args.",
    )
    return parser.parse_args()


def read_input_list(path: Path, base: Path | None) -> list[Path]:
    charts: list[Path] = []
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            chart = Path(line)
            if not chart.is_absolute() and base is not None:
                chart = base / chart
            charts.append(chart)
    return charts


def discover_charts(args: argparse.Namespace) -> list[Path]:
    roots = [Path(item) for item in args.inputs]
    input_base = Path(args.input_base) if args.input_base else None
    for list_path in args.input_list:
        roots.extend(read_input_list(Path(list_path), input_base))
    if not roots:
        roots = [Path("samples/simple_4k.osu"), Path("samples/simple_7k_ln.osu")]

    charts: list[Path] = []
    for root in roots:
        if root.is_file():
            if root.suffix.lower() in CHART_EXTENSIONS:
                charts.append(root)
            continue
        if root.is_dir():
            for path in sorted(root.rglob("*")):
                if path.is_file() and path.suffix.lower() in CHART_EXTENSIONS:
                    charts.append(path)
                    if args.limit and len(charts) >= args.limit:
                        return charts
            continue
        raise FileNotFoundError(f"input not found: {root}")

    charts = sorted(dict.fromkeys(charts))
    if args.limit:
        charts = charts[: args.limit]
    return charts


def check_paths(exe: Path, model: Path, charts: list[Path]) -> None:
    if not exe.is_file():
        raise FileNotFoundError(f"KeyWeaver executable not found: {exe}")
    if not model.is_file():
        raise FileNotFoundError(f"ONNX model not found: {model}")
    if not charts:
        raise FileNotFoundError("no chart files were discovered")


def safe_stem(path: Path) -> str:
    digest = hashlib.sha1(str(path.resolve()).encode("utf-8")).hexdigest()[:10]
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", path.stem).strip("._")
    return f"{stem or 'chart'}-{digest}"


def convert_value(raw: str, caster: type) -> Any:
    if caster is int:
        return int(raw)
    if caster is float:
        return float(raw)
    return raw


def parse_output(text: str) -> dict[str, Any]:
    metrics: dict[str, Any] = {}
    for key, (pattern, caster) in METRIC_PATTERNS.items():
        match = re.search(pattern, text, re.MULTILINE)
        if match:
            metrics[key] = convert_value(match.group(1), caster)
    for key, pattern in BOOL_PATTERNS.items():
        match = re.search(pattern, text, re.MULTILINE | re.IGNORECASE)
        if match:
            metrics[key] = match.group(1).lower() == "yes"
    for key, pattern in TEXT_PATTERNS.items():
        match = re.search(pattern, text, re.MULTILINE)
        if match:
            value = match.group(1).strip()
            if value and value != "-":
                metrics[key] = value
    return metrics


def keyweaver_command(args: argparse.Namespace, chart: Path, mode: str) -> list[str]:
    command = [
        str(Path(args.exe)),
        str(chart),
        "--target",
        str(args.target),
        "--batch",
        "--dry-run",
    ]
    if args.source is not None:
        command.extend(["--source", str(args.source)])
    if mode == "onnx":
        command.extend(
            [
                "--onnx-policy",
                str(Path(args.model)),
                "--onnx-provider",
                args.provider,
            ]
        )
        if not args.no_strict:
            command.append("--onnx-policy-strict")
    command.extend(args.extra_arg)
    return command


def run_command(command: list[str], timeout_sec: int = 120) -> tuple[int, float, str]:
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_sec,
        check=False,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    text = completed.stdout
    if completed.stderr:
        text += "\n[stderr]\n" + completed.stderr
    return completed.returncode, elapsed_ms, text


def run_measurement(
    args: argparse.Namespace,
    chart: Path,
    mode: str,
    repeat_index: int,
    log_dir: Path,
    record: bool,
) -> dict[str, Any] | None:
    command = keyweaver_command(args, chart, mode)
    exit_code, wall_ms, text = run_command(command)
    if not record:
        return None

    log_name = f"{safe_stem(chart)}-{mode}-r{repeat_index + 1}.txt"
    log_path = log_dir / log_name
    log_path.write_text(text, encoding="utf-8")
    metrics = parse_output(text)
    return {
        "chart": str(chart),
        "mode": mode,
        "repeat": repeat_index + 1,
        "exitCode": exit_code,
        "wallMs": round(wall_ms, 3),
        "command": command,
        "log": str(log_path),
        "metrics": metrics,
    }


def numeric_average(rows: list[dict[str, Any]], metric: str) -> float | int | None:
    values = [
        row["metrics"][metric]
        for row in rows
        if isinstance(row.get("metrics", {}).get(metric), (int, float))
    ]
    if not values:
        return None
    avg = sum(values) / len(values)
    if all(isinstance(value, int) for value in values):
        return int(round(avg))
    return round(avg, 3)


def row_average(rows: list[dict[str, Any]], key: str) -> float | None:
    values = [float(row[key]) for row in rows if isinstance(row.get(key), (int, float))]
    if not values:
        return None
    return round(sum(values) / len(values), 3)


def last_metric(rows: list[dict[str, Any]], metric: str) -> Any:
    for row in reversed(rows):
        if metric in row.get("metrics", {}):
            return row["metrics"][metric]
    return None


def format_delta(new_value: Any, old_value: Any) -> Any:
    if isinstance(new_value, (int, float)) and isinstance(old_value, (int, float)):
        return round(float(new_value) - float(old_value), 3)
    return ""


def make_summary(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    charts = sorted({row["chart"] for row in rows})
    for row in rows:
        grouped[(row["chart"], row["mode"])].append(row)

    summary: list[dict[str, Any]] = []
    for chart in charts:
        baseline = grouped.get((chart, "baseline"), [])
        onnx = grouped.get((chart, "onnx"), [])
        baseline_wall = row_average(baseline, "wallMs")
        onnx_wall = row_average(onnx, "wallMs")
        baseline_cli = numeric_average(baseline, "cliTotalMs")
        onnx_cli = numeric_average(onnx, "cliTotalMs")
        baseline_k = numeric_average(baseline, "kLikenessScore")
        onnx_k = numeric_average(onnx, "kLikenessScore")
        summary.append(
            {
                "chart": chart,
                "baselineExit": max((row["exitCode"] for row in baseline), default=""),
                "onnxExit": max((row["exitCode"] for row in onnx), default=""),
                "baselineWallMs": baseline_wall if baseline_wall is not None else "",
                "onnxWallMs": onnx_wall if onnx_wall is not None else "",
                "wallDeltaMs": format_delta(onnx_wall, baseline_wall),
                "baselineCliMs": baseline_cli if baseline_cli is not None else "",
                "onnxCliMs": onnx_cli if onnx_cli is not None else "",
                "baselineK": baseline_k if baseline_k is not None else "",
                "onnxK": onnx_k if onnx_k is not None else "",
                "kDelta": format_delta(onnx_k, baseline_k),
                "baselineSafety": numeric_average(baseline, "targetKSafetyScore") or "",
                "onnxSafety": numeric_average(onnx, "targetKSafetyScore") or "",
                "baselineCollisions": numeric_average(baseline, "sameTimeCollisions") or 0,
                "onnxCollisions": numeric_average(onnx, "sameTimeCollisions") or 0,
                "baselineLnConflicts": numeric_average(baseline, "lnConflicts") or 0,
                "onnxLnConflicts": numeric_average(onnx, "lnConflicts") or 0,
                "baselineCreatedJacks": numeric_average(baseline, "createdJacks") or 0,
                "onnxCreatedJacks": numeric_average(onnx, "createdJacks") or 0,
                "onnxLoaded": last_metric(onnx, "onnxLoaded"),
                "onnxProvider": last_metric(onnx, "onnxActiveProvider") or "",
                "onnxAttempted": numeric_average(onnx, "onnxAttemptedNotes") or 0,
                "onnxEvaluatedCandidates": numeric_average(onnx, "onnxEvaluatedCandidates") or 0,
                "onnxAccepted": numeric_average(onnx, "onnxAcceptedRelanes") or 0,
                "onnxFallback": numeric_average(onnx, "onnxFallbackRelanes") or 0,
                "onnxRejected": numeric_average(onnx, "onnxRejectedRelanes") or 0,
                "onnxSameLaneNoops": numeric_average(onnx, "onnxSameLaneNoops") or 0,
                "onnxRejectCollision": numeric_average(onnx, "onnxRejectCollision") or 0,
                "onnxRejectLnConflict": numeric_average(onnx, "onnxRejectLnConflict") or 0,
                "onnxRejectCreatedJack": numeric_average(onnx, "onnxRejectCreatedJack") or 0,
            }
        )
    return summary


def write_outputs(out_dir: Path, rows: list[dict[str, Any]]) -> tuple[Path, Path]:
    runs_path = out_dir / "runs.jsonl"
    with runs_path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    summary_path = out_dir / "summary.csv"
    with summary_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        for row in make_summary(rows):
            writer.writerow(row)
    return runs_path, summary_path


def main() -> int:
    args = parse_args()
    if args.repeat < 1:
        raise ValueError("--repeat must be at least 1")
    if args.warmup < 0:
        raise ValueError("--warmup must be non-negative")

    charts = discover_charts(args)
    exe = Path(args.exe)
    model = Path(args.model)
    check_paths(exe, model, charts)

    out_dir = Path(args.out_dir)
    log_dir = out_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, Any]] = []
    for chart in charts:
        print(f"[measure] {chart}")
        for mode in ("baseline", "onnx"):
            for _ in range(args.warmup):
                run_measurement(args, chart, mode, 0, log_dir, record=False)
            for repeat_index in range(args.repeat):
                row = run_measurement(args, chart, mode, repeat_index, log_dir, record=True)
                if row is not None:
                    rows.append(row)
                    status = "ok" if row["exitCode"] == 0 else f"exit={row['exitCode']}"
                    print(
                        f"  {mode} r{repeat_index + 1}: "
                        f"{status}, wall={row['wallMs']:.1f} ms"
                    )

    runs_path, summary_path = write_outputs(out_dir, rows)
    print(f"[measure] wrote {runs_path}")
    print(f"[measure] wrote {summary_path}")
    return 0 if all(row["exitCode"] == 0 for row in rows) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
