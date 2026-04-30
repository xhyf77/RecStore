from __future__ import annotations

import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Iterable


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def _percentile(sorted_values: list[float], pct: float) -> float:
    if not sorted_values:
        raise ValueError("cannot compute percentile for empty input")
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = (len(sorted_values) - 1) * pct
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return sorted_values[lo]
    frac = pos - lo
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * frac


def aggregate_metric_rows(rows: Iterable[dict[str, object]]) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str], list[float]] = defaultdict(list)
    for row in rows:
        grouped[(str(row["lane"]), str(row["metric"]))].append(float(row["value"]))

    out: list[dict[str, object]] = []
    for (lane, metric), values in sorted(grouped.items()):
        values_sorted = sorted(values)
        out.append(
            {
                "lane": lane,
                "metric": metric,
                "count": len(values_sorted),
                "mean": statistics.fmean(values_sorted),
                "std": statistics.stdev(values_sorted) if len(values_sorted) > 1 else 0.0,
                "p50": _percentile(values_sorted, 0.50),
                "p95": _percentile(values_sorted, 0.95),
                "min": values_sorted[0],
                "max": values_sorted[-1],
            }
        )
    return out


def write_summary_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = list(rows[0].keys())
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def warm_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    out = []
    for row in rows:
        warm = row.get("warmup_excluded", "0")
        if str(warm) in {"1", "True", "true"}:
            continue
        out.append(row)
    return out


def extract_main_metric(path: Path, column: str) -> float:
    rows = warm_rows(load_rows(path))
    values = [float(row[column]) for row in rows if row.get(column, "") != ""]
    if not values:
        raise ValueError(f"no values for column {column} in {path}")
    return statistics.fmean(values)


def extract_chain_metric(path: Path, column: str) -> float:
    rows = load_rows(path)
    values = [float(row[column]) for row in rows if row.get(column, "") not in {"", "nan", "NaN"}]
    if not values:
        raise ValueError(f"no values for column {column} in {path}")
    return statistics.fmean(values)
