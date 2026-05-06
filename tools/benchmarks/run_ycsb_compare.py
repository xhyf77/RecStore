from __future__ import annotations

import argparse
import csv
import re
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCHMARK_BIN = ROOT / "build/bin/benchmark_kv_engine"


@dataclass(frozen=True)
class EngineSpec:
    index_type: str
    value_store_type: str


KVENGINE_ALIASES: dict[str, EngineSpec] = {
    "dram_eh_dram": EngineSpec("DRAM_EXTENDIBLE_HASH", "DRAM_VALUE_STORE"),
    "dram_map_dram": EngineSpec("DRAM_UNORDERED_MAP", "DRAM_VALUE_STORE"),
    "dram_pet_dram": EngineSpec("DRAM_PET_HASH", "DRAM_VALUE_STORE"),
    "dram_eh_ssd": EngineSpec("DRAM_EXTENDIBLE_HASH", "SSD_VALUE_STORE"),
    "dram_eh_tiered": EngineSpec("DRAM_EXTENDIBLE_HASH", "TIERED_VALUE_STORE"),
    "ssd_eh_ssd": EngineSpec("SSD_EXTENDIBLE_HASH", "SSD_VALUE_STORE"),
}

SUMMARY_FIELDS = [
    "workload",
    "engine",
    "index_type",
    "value_store_type",
    "repeat",
    "record_count",
    "operation_count",
    "threads",
    "distribution",
    "zipfian_alpha",
    "read_mode",
    "phase",
    "exit_code",
    "load_runtime_sec",
    "load_operations",
    "load_throughput_ops_sec",
    "run_runtime_sec",
    "run_operations",
    "run_throughput_ops_sec",
    "run_read_operations",
    "run_update_operations",
    "data_path",
    "log_path",
    "error_tail",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run native benchmark_kv_engine YCSB comparisons."
    )
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--engines", nargs="+", default=["dram_eh_dram"])
    parser.add_argument("--workloads", nargs="+", default=["c"])
    parser.add_argument("--distribution", choices=["uniform", "zipfian"], default="uniform")
    parser.add_argument("--zipfian-alpha", type=float, default=0.9)
    parser.add_argument("--record-count", type=int, default=100_000)
    parser.add_argument(
        "--operation-count",
        type=int,
        default=0,
        help="Kept for CSV compatibility; timed benchmark ignores this value.",
    )
    parser.add_argument("--runtime-seconds", type=int, default=5)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--load-threads", type=int, default=0)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--read-mode", choices=["exists", "get"], default="exists")
    parser.add_argument("--dram-allocator", default="PERSIST_LOOP_SLAB")
    parser.add_argument("--ssd-io-backend", default="IOURING")
    parser.add_argument("--ssd-queue-depth", type=int, default=512)
    parser.add_argument("--dram-capacity-bytes", type=int, default=0)
    parser.add_argument("--ssd-capacity-bytes", type=int, default=0)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument("--skip-load", action="store_true")
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Extra benchmark_kv_engine argument, for example --extra-arg=--print_util=true.",
    )
    parser.add_argument(
        "--engine-prop",
        action="append",
        default=[],
        help="engine:gflag=value override, for example dram_eh_dram:dram_capacity_bytes=...",
    )
    return parser.parse_args()


def engine_spec(name: str) -> EngineSpec:
    if name in KVENGINE_ALIASES:
        return KVENGINE_ALIASES[name]
    raise ValueError(f"unknown KVEngine alias '{name}'")


def split_engine_props(items: list[str]) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for item in items:
        if ":" not in item:
            raise ValueError(f"--engine-prop must use engine:key=value: {item}")
        engine, prop = item.split(":", 1)
        if "=" not in prop:
            raise ValueError(f"--engine-prop must include key=value: {item}")
        out.setdefault(engine, []).append(prop)
    return out


def ensure_build() -> None:
    subprocess.run(
        ["cmake", "--build", "build", "--target", "benchmark_kv_engine", "-j"],
        cwd=ROOT,
        check=True,
    )


def sanitize(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value)


def normalize_workload(workload: str) -> str:
    lower = workload.lower()
    if lower == "workloada":
        return "a"
    if lower == "workloadb":
        return "b"
    if lower == "workloadc":
        return "c"
    if lower in {"a", "b", "c"}:
        return lower
    raise ValueError(f"unknown workload '{workload}', expected a/b/c")


def gflag(name: str, value: object) -> str:
    return f"--{name}={value}"


def benchmark_command(
    *,
    spec: EngineSpec,
    workload: str,
    data_path: Path,
    args: argparse.Namespace,
    engine_props: list[str],
) -> list[str]:
    cmd = [
        str(BENCHMARK_BIN),
        gflag("path", data_path),
        gflag("index_type", spec.index_type),
        gflag("value_store_type", spec.value_store_type),
        gflag("record_count", args.record_count),
        gflag("workload", workload),
        gflag("distribution", args.distribution),
        gflag("zipfian_alpha", args.zipfian_alpha),
        gflag("thread_num", args.threads),
        gflag("load_thread_num", args.load_threads),
        gflag("running_seconds", args.runtime_seconds),
        gflag("value_size", args.value_size),
        gflag("read_mode", args.read_mode),
        gflag("load", str(not args.skip_load).lower()),
        gflag("run", str(not args.skip_run).lower()),
        gflag("dram_allocator", args.dram_allocator),
        gflag("ssd_io_backend", args.ssd_io_backend),
        gflag("ssd_queue_depth", args.ssd_queue_depth),
    ]
    if args.dram_capacity_bytes:
        cmd.append(gflag("dram_capacity_bytes", args.dram_capacity_bytes))
    if args.ssd_capacity_bytes:
        cmd.append(gflag("ssd_capacity_bytes", args.ssd_capacity_bytes))
    for prop in engine_props:
        key, value = prop.split("=", 1)
        cmd.append(gflag(key, value))
    cmd.extend(args.extra_arg)
    return cmd


def parse_result_line(line: str) -> dict[str, str]:
    parts = line.strip().split()
    out: dict[str, str] = {}
    for part in parts[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            out[key] = value
    return out


def extract_metrics(output: str) -> tuple[dict[str, str], dict[str, str]]:
    load: dict[str, str] = {}
    run: dict[str, str] = {}
    for line in output.splitlines():
        if line.startswith("YCSB_LOAD_RESULT "):
            load = parse_result_line(line)
        elif line.startswith("YCSB_RESULT "):
            run = parse_result_line(line)
    return load, run


def error_tail(output: str, exit_code: int) -> str:
    if exit_code == 0:
        return ""
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    return " | ".join(lines[-5:])[:1000]


def run_one(
    *,
    engine: str,
    spec: EngineSpec,
    workload: str,
    repeat: int,
    args: argparse.Namespace,
    engine_props: list[str],
) -> dict[str, object]:
    run_name = f"{sanitize(workload)}_{sanitize(args.distribution)}_{sanitize(engine)}_r{repeat}"
    data_path = args.output_dir / "data" / run_name
    log_path = args.output_dir / "logs" / f"{run_name}.log"
    if data_path.exists() and not args.keep_data:
        shutil.rmtree(data_path)
    data_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    cmd = benchmark_command(
        spec=spec,
        workload=workload,
        data_path=data_path,
        args=args,
        engine_props=engine_props,
    )
    start = time.perf_counter()
    proc = subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    elapsed = time.perf_counter() - start
    output = " ".join(cmd) + "\n\n" + proc.stdout + f"\nwall_runtime_sec={elapsed}\n"
    log_path.write_text(output, encoding="utf-8")
    load, run = extract_metrics(proc.stdout)

    row = {
        "workload": workload,
        "engine": engine,
        "index_type": spec.index_type,
        "value_store_type": spec.value_store_type,
        "repeat": repeat,
        "record_count": args.record_count,
        "operation_count": args.operation_count,
        "threads": args.threads,
        "distribution": args.distribution,
        "zipfian_alpha": args.zipfian_alpha,
        "read_mode": args.read_mode,
        "phase": "load-run",
        "exit_code": proc.returncode,
        "load_runtime_sec": load.get("seconds", ""),
        "load_operations": load.get("ops", ""),
        "load_throughput_ops_sec": load.get("throughput_ops_sec", ""),
        "run_runtime_sec": run.get("runtime_s", ""),
        "run_operations": run.get("ops", ""),
        "run_throughput_ops_sec": run.get("throughput_ops_sec", ""),
        "run_read_operations": run.get("read_ops", ""),
        "run_update_operations": run.get("update_ops", ""),
        "data_path": str(data_path),
        "log_path": str(log_path),
        "error_tail": error_tail(proc.stdout, proc.returncode),
    }
    if not args.keep_data:
        shutil.rmtree(data_path, ignore_errors=True)
        Path(str(data_path) + "_index.db").unlink(missing_ok=True)
        Path(str(data_path) + "_value.db").unlink(missing_ok=True)
    return row


def write_summary(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    if args.build:
        ensure_build()
    if not BENCHMARK_BIN.exists():
        raise FileNotFoundError(
            f"{BENCHMARK_BIN} does not exist; build target benchmark_kv_engine first"
        )
    engine_props = split_engine_props(args.engine_prop)
    rows: list[dict[str, object]] = []
    for repeat in range(args.repeat):
        for workload_arg in args.workloads:
            workload = normalize_workload(workload_arg)
            for engine in args.engines:
                spec = engine_spec(engine)
                props = engine_props.get(engine, [])
                row = run_one(
                    engine=engine,
                    spec=spec,
                    workload=workload,
                    repeat=repeat,
                    args=args,
                    engine_props=props,
                )
                rows.append(row)
                print(
                    f"{workload} {args.distribution} {engine} r{repeat}: "
                    f"exit={row['exit_code']} load={row['load_throughput_ops_sec']} "
                    f"run={row['run_throughput_ops_sec']}"
                )
                write_summary(args.output_dir / "summary.csv", rows)
    return 0 if all(int(row["exit_code"]) == 0 for row in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
