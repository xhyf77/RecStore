from __future__ import annotations

import argparse
import csv
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMPARE = ROOT / "tools/benchmarks/run_ycsb_compare.py"

DEFAULT_ENGINES = [
    "dram_eh_dram",
    "dram_map_dram",
    "dram_pet_dram",
    "dram_eh_ssd",
    "dram_eh_tiered",
    "ssd_eh_ssd",
]

ENGINE_LABELS = {
    "dram_eh_dram": "DRAM_EH+DRAM",
    "dram_map_dram": "DRAM_MAP+DRAM",
    "dram_pet_dram": "DRAM_PET+DRAM",
    "dram_eh_ssd": "DRAM_EH+SSD",
    "dram_eh_tiered": "DRAM_EH+TIERED",
    "ssd_eh_ssd": "SSD_EH+SSD",
}

DISTRIBUTION_LABELS = {
    "uniform": "uniform",
    "zipfian": "zipfian(alpha=0.9)",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run timed YCSB workloads for KVEngine combinations and render a bar chart."
    )
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--engines", nargs="+", default=DEFAULT_ENGINES)
    parser.add_argument("--workloads", nargs="+", default=["a", "b", "c"])
    parser.add_argument("--distributions", nargs="+", default=["uniform", "zipfian"])
    parser.add_argument("--zipfian-alpha", type=float, default=0.9)
    parser.add_argument("--record-count", type=int, default=100_000_000)
    parser.add_argument("--runtime-seconds", type=int, default=5)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--load-threads", type=int, default=0)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--read-mode", choices=["exists", "get"], default="exists")
    parser.add_argument("--dram-capacity-bytes", type=int, default=0)
    parser.add_argument("--ssd-capacity-bytes", type=int, default=0)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "results/storage-YCSB")
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Extra benchmark_kv_engine argument; per-run --path is managed by run_ycsb_compare.py.",
    )
    parser.add_argument("--engine-prop", action="append", default=[])
    parser.add_argument("--skip-run", action="store_true", help="Only render existing summary.csv.")
    return parser.parse_args()


def run_compare_once(args: argparse.Namespace, output_dir: Path, distribution: str) -> None:
    cmd = [
        sys.executable,
        str(COMPARE),
        "--engines",
        *args.engines,
        "--workloads",
        *args.workloads,
        "--record-count",
        str(args.record_count),
        "--threads",
        str(args.threads),
        "--load-threads",
        str(args.load_threads),
        "--repeat",
        str(args.repeat),
        "--runtime-seconds",
        str(args.runtime_seconds),
        "--value-size",
        str(args.value_size),
        "--read-mode",
        args.read_mode,
        "--output-dir",
        str(output_dir),
        "--distribution",
        distribution,
        "--zipfian-alpha",
        str(args.zipfian_alpha),
    ]
    if args.build:
        cmd.append("--build")
    if args.keep_data:
        cmd.append("--keep-data")
    for extra_arg in args.extra_arg:
        cmd.append(f"--extra-arg={extra_arg}")

    engine_props = [
        f"dram_eh_dram:dram_capacity_bytes={args.dram_capacity_bytes}",
        f"dram_map_dram:dram_capacity_bytes={args.dram_capacity_bytes}",
        f"dram_pet_dram:dram_capacity_bytes={args.dram_capacity_bytes}",
        f"dram_eh_tiered:dram_capacity_bytes={args.dram_capacity_bytes}",
        f"dram_eh_ssd:ssd_capacity_bytes={args.ssd_capacity_bytes}",
        f"dram_eh_tiered:ssd_capacity_bytes={args.ssd_capacity_bytes}",
        f"ssd_eh_ssd:ssd_capacity_bytes={args.ssd_capacity_bytes}",
    ]
    engine_props.extend(args.engine_prop)
    for prop in engine_props:
        engine = prop.split(":", 1)[0]
        value = prop.split("=", 1)[1] if "=" in prop else ""
        if engine in args.engines and value not in {"", "0"}:
            cmd.extend(["--engine-prop", prop])

    subprocess.run(cmd, cwd=ROOT, check=True)


def run_compare(args: argparse.Namespace) -> None:
    all_rows: list[dict[str, str]] = []
    logs_dir = args.output_dir / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="recstore_ycsb_dist_", dir="/tmp") as tmp:
        tmp_root = Path(tmp)
        for distribution in args.distributions:
            dist_dir = tmp_root / distribution
            run_compare_once(args, dist_dir, distribution)
            for row in load_summary(dist_dir / "summary.csv"):
                row["distribution"] = distribution
                old_log = Path(row["log_path"])
                new_log = logs_dir / f"{distribution}_{old_log.name}"
                if old_log.exists():
                    shutil.copy2(old_log, new_log)
                    row["log_path"] = str(new_log)
                row["data_path"] = ""
                all_rows.append(row)
            if args.keep_data:
                target = args.output_dir / "data" / distribution
                if target.exists():
                    shutil.rmtree(target)
                shutil.copytree(dist_dir / "data", target)

    write_summary(args.output_dir / "summary.csv", all_rows)


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    base_fields = ["distribution"]
    fieldnames = base_fields + [name for name in rows[0].keys() if name not in base_fields]
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def load_summary(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def write_aggregate(rows: list[dict[str, str]], path: Path) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str, str], list[dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault((row.get("distribution", "default"), row["workload"], row["engine"]), []).append(row)

    out: list[dict[str, object]] = []
    for (distribution, workload, engine), group in sorted(grouped.items()):
        ok = [r for r in group if r["exit_code"] == "0"]
        run_values = [float(r["run_throughput_ops_sec"]) for r in ok if r["run_throughput_ops_sec"]]
        load_values = [float(r["load_throughput_ops_sec"]) for r in ok if r["load_throughput_ops_sec"]]
        run_ops = [float(r["run_operations"]) for r in ok if r["run_operations"]]
        out.append(
            {
                "distribution": distribution,
                "distribution_label": DISTRIBUTION_LABELS.get(distribution, distribution),
                "workload": workload,
                "engine": engine,
                "engine_label": ENGINE_LABELS.get(engine, engine),
                "runs": len(group),
                "successes": len(ok),
                "avg_load_ops_sec": sum(load_values) / len(load_values) if load_values else "",
                "avg_run_ops_sec": sum(run_values) / len(run_values) if run_values else "",
                "avg_run_operations": sum(run_ops) / len(run_ops) if run_ops else "",
            }
        )

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        fieldnames = [
            "distribution",
            "distribution_label",
            "workload",
            "engine",
            "engine_label",
            "runs",
            "successes",
            "avg_load_ops_sec",
            "avg_run_ops_sec",
            "avg_run_operations",
        ]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(out)
    return out


def render_chart(rows: list[dict[str, object]], svg_path: Path) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.ticker import FuncFormatter
    except ImportError as exc:
        raise SystemExit(
            "matplotlib is required to render the YCSB chart. "
            "Install it with: python3 -m pip install matplotlib"
        ) from exc

    categories = sorted({(str(r["distribution"]), str(r["workload"])) for r in rows})
    engines = [e for e in DEFAULT_ENGINES if any(r["engine"] == e for r in rows)]
    engines.extend(sorted({str(r["engine"]) for r in rows} - set(engines)))
    values = {
        (str(r["distribution"]), str(r["workload"]), str(r["engine"])): float(r["avg_run_ops_sec"] or 0)
        for r in rows
    }

    x = list(range(len(categories)))
    width = 0.82 / max(len(engines), 1)
    fig_w = max(14, 2.4 * len(categories))
    fig, ax = plt.subplots(figsize=(fig_w, 7), constrained_layout=True)
    colors = ["#2563eb", "#16a34a", "#dc2626", "#9333ea", "#ea580c", "#0891b2", "#4b5563"]

    for idx, engine in enumerate(engines):
        offset = (idx - (len(engines) - 1) / 2) * width
        heights = [values.get((distribution, workload, engine), 0.0) for distribution, workload in categories]
        bars = ax.bar(
            [pos + offset for pos in x],
            heights,
            width=width,
            label=ENGINE_LABELS.get(engine, engine),
            color=colors[idx % len(colors)],
        )
        ax.bar_label(
            bars,
            labels=[format_ops(v) if v > 0 else "" for v in heights],
            rotation=75,
            padding=3,
            fontsize=8,
        )

    ax.set_title("YCSB timed-run throughput by KVEngine and key distribution", fontsize=16, fontweight="bold")
    ax.set_xlabel("YCSB workload / key distribution")
    ax.set_ylabel("Run throughput (ops/sec)")
    ax.set_xticks(x)
    ax.set_xticklabels(
        [f"{workload}\n{DISTRIBUTION_LABELS.get(distribution, distribution)}" for distribution, workload in categories],
        fontsize=9,
    )
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: format_ops(v)))
    ax.grid(axis="y", color="#e5e7eb", linewidth=0.8)
    ax.set_axisbelow(True)
    ax.legend(ncols=3, fontsize=9)

    svg_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(svg_path, format="svg")
    fig.savefig(svg_path.with_suffix(".png"), dpi=160)
    plt.close(fig)


def format_ops(value: float) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f}M"
    if value >= 1_000:
        return f"{value / 1_000:.0f}K"
    return f"{value:.0f}"


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if not args.skip_run:
        run_compare(args)
        if not args.keep_data:
            shutil.rmtree(args.output_dir / "data", ignore_errors=True)
    summary = args.output_dir / "summary.csv"
    rows = load_summary(summary)
    aggregate = write_aggregate(rows, args.output_dir / "kvengine_workload_summary.csv")
    render_chart(aggregate, args.output_dir / "kvengine_ycsb_run_throughput.svg")
    print(f"summary: {summary}")
    print(f"aggregate: {args.output_dir / 'kvengine_workload_summary.csv'}")
    print(f"chart: {args.output_dir / 'kvengine_ycsb_run_throughput.svg'}")
    print(f"chart_png: {args.output_dir / 'kvengine_ycsb_run_throughput.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
