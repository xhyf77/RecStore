from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from .aggregate import extract_chain_metric, extract_main_metric, write_summary_csv, aggregate_metric_rows
from .common import DEFAULT_OUTPUT_ROOT, DEFAULT_REMOTE_HOST, REPO_ROOT, LaneSpec, SuiteSpec, ensure_dir, write_json
from .remote import remote_cleanup_ps_and_paths, remote_run_worker, remote_start_ps, remote_sync_repo, stop_process
from .remote import wait_remote_ports
from .runtime import build_remote_recstore_runtime


@dataclass
class RunArtifacts:
    lane: LaneSpec
    repeat: int
    run_id: str
    output_dir: Path
    main_csv: Path
    chain_csv: Path | None


def _main_csv_path(lane: LaneSpec, output_dir: Path) -> Path:
    if lane.backend == 'recstore':
        return output_dir / 'recstore_main.csv'
    return output_dir / 'torchrec_main.csv'


def _chain_csv_path(lane: LaneSpec, output_dir: Path) -> Path | None:
    if lane.backend != 'recstore':
        return None
    return output_dir / 'recstore_embupdate.csv'


def _build_run_id(suite_name: str, lane: LaneSpec, tag: str, repeat: int) -> str:
    return f'{suite_name}-{lane.slug}-{tag}-r{repeat:02d}'


def _base_args(lane: LaneSpec, run_id: str) -> list[str]:
    rdzv_id = lane.rdzv_id or run_id
    args = [
        sys.executable,
        str(REPO_ROOT / 'model_zoo/rs_demo/run_mock_stress.py'),
        '--backend', lane.backend,
        '--steps', str(lane.steps),
        '--warmup-steps', str(lane.warmup_steps),
        '--batch-size', str(lane.batch_size),
        '--num-embeddings', str(lane.num_embeddings),
        '--embedding-dim', str(lane.embedding_dim),
        '--run-id', run_id,
        '--output-root', str(DEFAULT_OUTPUT_ROOT),
        '--nnodes', str(lane.nnodes),
        '--node-rank', str(lane.node_rank),
        '--nproc-per-node', str(lane.nproc_per_node),
        '--master-addr', lane.master_addr,
        '--master-port', str(lane.master_port),
        '--rdzv-id', rdzv_id,
    ]
    if lane.no_start_server:
        args.append('--no-start-server')
    if lane.server_host:
        args.extend(['--server-host', lane.server_host])
    if lane.server_port0 is not None:
        args.extend(['--server-port0', str(lane.server_port0)])
    if lane.server_port1 is not None:
        args.extend(['--server-port1', str(lane.server_port1)])
    if lane.recstore_runtime_dir:
        args.extend(['--recstore-runtime-dir', lane.recstore_runtime_dir])
    if lane.torchrec_dist_mode:
        args.extend(['--torchrec-dist-mode', lane.torchrec_dist_mode])
    return args


def _run_local(command: list[str]) -> None:
    subprocess.run(command, cwd=str(REPO_ROOT), check=True)


def _run_remote_command(command: list[str]) -> str:
    return 'cd /app/RecStore && ' + ' '.join(str(part) for part in command)


def run_lane(lane: LaneSpec, suite_name: str, repeat: int, tag: str, *, dry_run: bool = False, branch: str | None = None) -> RunArtifacts:
    run_id = _build_run_id(suite_name, lane, tag, repeat)
    output_dir = DEFAULT_OUTPUT_ROOT / 'outputs' / run_id
    main_csv = _main_csv_path(lane, output_dir)
    chain_csv = _chain_csv_path(lane, output_dir)

    local_args = _base_args(lane, run_id)
    remote_worker_proc = None
    remote_ps_proc = None
    try:
        if lane.needs_remote_ps:
            runtime_slug = run_id + '-runtime'
            runtime_config = build_remote_recstore_runtime(runtime_slug, path_name=runtime_slug)
            local_args[local_args.index('--recstore-runtime-dir') + 1] = str(runtime_config.parent)
            if not dry_run:
                remote_cleanup_ps_and_paths([f'/tmp/{runtime_slug}*'])
                remote_ps_proc = remote_start_ps(str(runtime_config))
                wait_remote_ports('10.0.2.68', [15123, 15124], timeout_s=20.0)
        if lane.remote_worker:
            remote_args = _base_args(lane, run_id)
            remote_args[remote_args.index('--node-rank') + 1] = '1'
            if lane.recstore_runtime_dir:
                runtime_slug = run_id + '-runtime'
                remote_args[remote_args.index('--recstore-runtime-dir') + 1] = str((DEFAULT_OUTPUT_ROOT / 'runtime' / runtime_slug))
            if not dry_run:
                remote_worker_proc = remote_run_worker(_run_remote_command(remote_args))
        if not dry_run:
            _run_local(local_args)
            if remote_worker_proc is not None:
                code = remote_worker_proc.wait(timeout=1200)
                if code != 0:
                    raise RuntimeError(f'remote worker exited with code {code}')
    finally:
        stop_process(remote_ps_proc)
        stop_process(remote_worker_proc)

    return RunArtifacts(lane=lane, repeat=repeat, run_id=run_id, output_dir=output_dir, main_csv=main_csv, chain_csv=chain_csv)


def summarize_artifacts(artifacts: list[RunArtifacts], summary_dir: Path) -> None:
    metric_rows: list[dict[str, object]] = []
    for art in artifacts:
        for metric in art.lane.metrics:
            if metric.source == 'main_csv':
                try:
                    value = extract_main_metric(art.main_csv, metric.column)
                except ValueError:
                    continue
            else:
                if art.chain_csv is None:
                    continue
                try:
                    value = extract_chain_metric(art.chain_csv, metric.column)
                except ValueError:
                    continue
            metric_rows.append({'lane': art.lane.name, 'metric': metric.name, 'repeat': art.repeat, 'run_id': art.run_id, 'value': value})
    ensure_dir(summary_dir)
    write_json(summary_dir / 'raw_metrics.json', metric_rows)
    write_summary_csv(summary_dir / 'raw_metrics.csv', metric_rows)
    write_summary_csv(summary_dir / 'aggregated_metrics.csv', aggregate_metric_rows(metric_rows))


def run_suite(
    suite: SuiteSpec,
    repeat: int,
    tag: str,
    *,
    dry_run: bool = False,
    branch: str | None = None,
    skip_remote_sync: bool = False,
) -> Path:
    suite_root = DEFAULT_OUTPUT_ROOT / 'benchmarks' / suite.name / tag
    ensure_dir(suite_root)
    artifacts: list[RunArtifacts] = []
    manifest: list[dict[str, object]] = []
    if any(lane.needs_remote_sync for lane in suite.lanes) and not dry_run and not skip_remote_sync:
        remote_sync_repo(branch=branch)
    for rep in range(1, repeat + 1):
        for lane in suite.lanes:
            art = run_lane(lane, suite.name, rep, tag, dry_run=dry_run, branch=branch)
            artifacts.append(art)
            manifest.append(
                {
                    'lane': lane.name,
                    'repeat': rep,
                    'run_id': art.run_id,
                    'backend': lane.backend,
                    'main_csv': str(art.main_csv),
                    'chain_csv': str(art.chain_csv) if art.chain_csv else '',
                    'needs_remote_ps': lane.needs_remote_ps,
                    'remote_worker': lane.remote_worker,
                }
            )
    write_json(suite_root / 'manifest.json', manifest)
    if not dry_run:
        summarize_artifacts(artifacts, suite_root / 'summary')
    return suite_root
