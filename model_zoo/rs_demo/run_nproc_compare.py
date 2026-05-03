#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RecStoreLane:
    slug: str
    label: str
    requires_nproc_gt_one: bool = False
    enable_single_node_fast_path: bool = False
    single_node_ps_backend: str = "local_shm"
    enable_gpu_cache: bool = False


@dataclass(frozen=True)
class TorchRecLane:
    slug: str
    label: str
    torchrec_dist_mode: str = "replicated"
    requires_nproc_gt_one: bool = False


@dataclass(frozen=True)
class CompareScenario:
    batch_size: int
    nproc: int
    repeat: int
    recstore_lane: RecStoreLane
    torchrec_lane: TorchRecLane

    @property
    def is_legacy_tag(self) -> bool:
        return (
            self.recstore_lane.slug == "legacy-brpc"
            and self.torchrec_lane.slug == "replicated"
        )

    @property
    def tag(self) -> str:
        base = f"b{self.batch_size}_n{self.nproc}_r{self.repeat}"
        if self.is_legacy_tag:
            return base
        return f"{base}_{self.recstore_lane.slug}_vs_{self.torchrec_lane.slug}"

    @property
    def scenario(self) -> str:
        return f"{self.recstore_lane.slug}_vs_{self.torchrec_lane.slug}"


RECSTORE_LANES: dict[str, RecStoreLane] = {
    "legacy-brpc": RecStoreLane(
        slug="legacy-brpc",
        label="RecStore-Legacy-BRPC",
    ),
    "single-node-local-shm": RecStoreLane(
        slug="single-node-local-shm",
        label="RecStore-SingleNode-LocalSHM",
        requires_nproc_gt_one=True,
        enable_single_node_fast_path=True,
        single_node_ps_backend="local_shm",
    ),
    "single-node-hierkv": RecStoreLane(
        slug="single-node-hierkv",
        label="RecStore-SingleNode-HierKV",
        requires_nproc_gt_one=True,
        enable_single_node_fast_path=True,
        single_node_ps_backend="hierkv",
    ),
}

TORCHREC_LANES: dict[str, TorchRecLane] = {
    "replicated": TorchRecLane(
        slug="replicated",
        label="TorchRec-Replicated",
    ),
    "fair-remote": TorchRecLane(
        slug="fair-remote",
        label="TorchRec-FairRemote",
        torchrec_dist_mode="fair_remote",
        requires_nproc_gt_one=True,
    ),
}


def _known_values(values: list[str], known: dict[str, object], flag: str) -> list[str]:
    unknown = [value for value in values if value not in known]
    if unknown:
        raise ValueError(
            f"unknown {flag} value(s): {', '.join(unknown)}. "
            f"known values: {', '.join(sorted(known))}"
        )
    return values


def build_scenarios(
    *,
    batch_sizes: list[int],
    nprocs: list[int],
    repeats: int,
    recstore_lanes: list[str],
    torchrec_lanes: list[str],
    include_invalid: bool,
) -> list[CompareScenario]:
    recstore_lanes = _known_values(recstore_lanes, RECSTORE_LANES, "--recstore-lanes")
    torchrec_lanes = _known_values(torchrec_lanes, TORCHREC_LANES, "--torchrec-lanes")

    scenarios: list[CompareScenario] = []
    for batch_size in batch_sizes:
        for nproc in nprocs:
            for recstore_lane_name in recstore_lanes:
                recstore_lane = RECSTORE_LANES[recstore_lane_name]
                if recstore_lane.requires_nproc_gt_one and nproc <= 1 and not include_invalid:
                    continue
                for torchrec_lane_name in torchrec_lanes:
                    torchrec_lane = TORCHREC_LANES[torchrec_lane_name]
                    if (
                        torchrec_lane.requires_nproc_gt_one
                        and nproc <= 1
                        and not include_invalid
                    ):
                        continue
                    for repeat_idx in range(repeats):
                        scenarios.append(
                            CompareScenario(
                                batch_size=batch_size,
                                nproc=nproc,
                                repeat=repeat_idx,
                                recstore_lane=recstore_lane,
                                torchrec_lane=torchrec_lane,
                            )
                        )
    return scenarios


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run rs_demo RecStore vs TorchRec comparison over batch/nproc grid."
    )
    parser.add_argument(
        "--batch-sizes",
        type=int,
        nargs="+",
        default=[256, 512, 1024],
    )
    parser.add_argument(
        "--nprocs",
        type=int,
        nargs="+",
        default=[1, 2, 4],
    )
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--steps", type=int, default=20)
    parser.add_argument("--warmup-steps", type=int, default=3)
    parser.add_argument("--num-embeddings", type=int, default=10000)
    parser.add_argument("--embedding-dim", type=int, default=128)
    parser.add_argument(
        "--output-dir",
        type=str,
        default="/tmp/rs_demo_nproc_compare",
    )
    parser.add_argument("--master-port-base", type=int, default=29600)
    parser.add_argument(
        "--recstore-lanes",
        nargs="+",
        default=["legacy-brpc"],
        choices=sorted(RECSTORE_LANES),
        help=(
            "RecStore lanes to compare. Default keeps the legacy local BRPC path. "
            "single-node-* lanes require nproc > 1 unless --include-invalid-scenarios is set."
        ),
    )
    parser.add_argument(
        "--torchrec-lanes",
        nargs="+",
        default=["replicated"],
        choices=sorted(TORCHREC_LANES),
        help=(
            "TorchRec lanes to compare. Default keeps the legacy replicated path. "
            "fair-remote requires nproc > 1 unless --include-invalid-scenarios is set."
        ),
    )
    parser.add_argument(
        "--include-invalid-scenarios",
        action="store_true",
        default=False,
        help="Do not skip lane/nproc combinations that the underlying runner will reject.",
    )
    parser.add_argument(
        "--gpu-cache-capacity",
        type=int,
        default=0,
        help="GPU cache capacity for RecStore lanes that enable GPU cache.",
    )
    parser.add_argument("--torchrec-profiler", action="store_true", default=False)
    parser.add_argument("--torchrec-profiler-active", type=int, default=1)
    parser.add_argument("--torchrec-profiler-repeat", type=int, default=1)
    return parser


def _run(cmd: list[str], cwd: Path) -> None:
    res = subprocess.run(
        cmd,
        cwd=str(cwd),
        text=True,
        capture_output=True,
        check=False,
    )
    if res.returncode != 0:
        raise RuntimeError(
            "command failed\n"
            f"cmd={' '.join(cmd)}\n"
            f"stdout:\n{res.stdout}\n"
            f"stderr:\n{res.stderr}"
        )


def _is_completed(compare_csv: Path, torchrec_agg_csv: Path) -> bool:
    return compare_csv.exists() and torchrec_agg_csv.exists()


def _read_single_row(path: Path) -> dict[str, str]:
    with path.open("r", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError(f"expected rows in csv: {path}")
    return rows[0]


def _read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def _add_scenario_fields(
    row: dict[str, str | int | float],
    scenario: CompareScenario,
) -> dict[str, str | int | float]:
    row["batch_size"] = scenario.batch_size
    row["nproc"] = scenario.nproc
    row["repeat"] = scenario.repeat
    row["scenario"] = scenario.scenario
    row["recstore_lane"] = scenario.recstore_lane.slug
    row["recstore_label"] = scenario.recstore_lane.label
    row["torchrec_lane"] = scenario.torchrec_lane.slug
    row["torchrec_label"] = scenario.torchrec_lane.label
    row["torchrec_dist_mode"] = scenario.torchrec_lane.torchrec_dist_mode
    return row


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    repo_root = Path(__file__).resolve().parents[2]
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    summary_rows: list[dict[str, str | int | float]] = []
    compare_rows: list[dict[str, str | int | float]] = []

    scenarios = build_scenarios(
        batch_sizes=args.batch_sizes,
        nprocs=args.nprocs,
        repeats=args.repeat,
        recstore_lanes=args.recstore_lanes,
        torchrec_lanes=args.torchrec_lanes,
        include_invalid=args.include_invalid_scenarios,
    )
    include_context_columns = any(not scenario.is_legacy_tag for scenario in scenarios)

    for run_idx, scenario in enumerate(scenarios, start=1):
        batch_size = scenario.batch_size
        nproc = scenario.nproc
        tag = scenario.tag
        recstore_csv = output_dir / f"{tag}_recstore.csv"
        recstore_jsonl = output_dir / f"{tag}_recstore.jsonl"
        recstore_log = output_dir / f"{tag}_ps_server.log"
        recstore_agg_csv = output_dir / f"{tag}_recstore_agg.csv"
        torchrec_csv = output_dir / f"{tag}_torchrec.csv"
        torchrec_agg_csv = output_dir / f"{tag}_torchrec_agg.csv"
        torchrec_trace_dir = output_dir / f"{tag}_torchrec_traces"
        torchrec_trace_csv = output_dir / f"{tag}_torchrec_trace.csv"
        compare_csv = output_dir / f"{tag}_compare.csv"

        if _is_completed(compare_csv, torchrec_agg_csv):
            print(f"[rs_demo] skip completed {tag}")
            agg_row = _read_single_row(torchrec_agg_csv)
            _add_scenario_fields(agg_row, scenario)
            summary_rows.append(agg_row)
            for row in _read_rows(compare_csv):
                _add_scenario_fields(row, scenario)
                compare_rows.append(row)
            continue

        common_args = [
            "--steps",
            str(args.steps),
            "--warmup-steps",
            str(args.warmup_steps),
            "--batch-size",
            str(batch_size),
            "--num-embeddings",
            str(args.num_embeddings),
            "--embedding-dim",
            str(args.embedding_dim),
        ]

        recstore_cmd = [
            sys.executable,
            str(repo_root / "model_zoo/rs_demo/run_mock_stress.py"),
            *common_args,
            "--jsonl",
            str(recstore_jsonl),
            "--csv",
            str(recstore_csv),
            "--recstore-main-csv",
            str(recstore_csv),
            "--recstore-main-agg-csv",
            str(recstore_agg_csv),
            "--server-log",
            str(recstore_log),
        ]
        if scenario.recstore_lane.enable_single_node_fast_path:
            recstore_cmd.extend(
                [
                    "--nnodes",
                    "1",
                    "--nproc-per-node",
                    str(nproc),
                    "--master-port",
                    str(args.master_port_base + run_idx),
                    "--enable-single-node-distributed-fast-path",
                    "--single-node-ps-backend",
                    scenario.recstore_lane.single_node_ps_backend,
                ]
            )
        if scenario.recstore_lane.enable_gpu_cache:
            recstore_cmd.extend(
                [
                    "--enable-gpu-cache",
                    "--gpu-cache-capacity",
                    str(args.gpu_cache_capacity),
                ]
            )
        _run(recstore_cmd, repo_root)

        torchrec_cmd = [
            sys.executable,
            str(repo_root / "model_zoo/rs_demo/run_mock_stress.py"),
            "--backend",
            "torchrec",
            "--nproc",
            str(nproc),
            "--nproc-per-node",
            str(nproc),
            "--master-port",
            str(args.master_port_base + run_idx),
            "--torchrec-dist-mode",
            scenario.torchrec_lane.torchrec_dist_mode,
            *common_args,
            "--no-start-server",
            "--torchrec-main-csv",
            str(torchrec_csv),
            "--torchrec-main-agg-csv",
            str(torchrec_agg_csv),
            "--torchrec-trace-dir",
            str(torchrec_trace_dir),
            "--torchrec-trace-csv",
            str(torchrec_trace_csv),
            "--torchrec-compare-recstore-csv",
            str(recstore_csv),
            "--torchrec-compare-csv",
            str(compare_csv),
        ]
        if args.torchrec_profiler:
            torchrec_cmd.extend(
                [
                    "--torchrec-profiler",
                    "--torchrec-profiler-active",
                    str(args.torchrec_profiler_active),
                    "--torchrec-profiler-repeat",
                    str(args.torchrec_profiler_repeat),
                ]
            )
        _run(torchrec_cmd, repo_root)

        agg_row = _read_single_row(torchrec_agg_csv)
        _add_scenario_fields(agg_row, scenario)
        summary_rows.append(agg_row)

        with compare_csv.open("r", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                _add_scenario_fields(row, scenario)
                compare_rows.append(row)

    summary_path = output_dir / "torchrec_grid_summary.csv"
    with summary_path.open("w", encoding="utf-8", newline="") as f:
        leading = [
            "batch_size",
            "nproc",
            "repeat",
        ]
        context_fields = [
            "scenario",
            "recstore_lane",
            "recstore_label",
            "torchrec_lane",
            "torchrec_label",
            "torchrec_dist_mode",
        ]
        if include_context_columns:
            leading.extend(context_fields)
        all_fields = sorted({key for row in summary_rows for key in row.keys()})
        excluded = set(context_fields if not include_context_columns else [])
        fieldnames = leading + [
            key for key in all_fields if key not in leading and key not in excluded
        ]
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(summary_rows)

    compare_path = output_dir / "recstore_torchrec_grid_compare.csv"
    with compare_path.open("w", encoding="utf-8", newline="") as f:
        fieldnames = [
            "batch_size",
            "nproc",
            "repeat",
        ]
        if include_context_columns:
            fieldnames.extend(
                [
                    "scenario",
                    "recstore_lane",
                    "recstore_label",
                    "torchrec_lane",
                    "torchrec_label",
                    "torchrec_dist_mode",
                ]
            )
        fieldnames.extend(
            [
                "metric",
                "recstore_ms",
                "torchrec_ms",
                "delta_ms",
                "delta_ratio",
            ]
        )
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(compare_rows)

    print(f"[rs_demo] output_dir: {output_dir}")
    print(f"[rs_demo] torchrec_grid_summary: {summary_path}")
    print(f"[rs_demo] recstore_torchrec_grid_compare: {compare_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
