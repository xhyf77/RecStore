from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.benchmarks.lanes import build_suites
from tools.benchmarks.runner import run_suite


def main() -> int:
    parser = argparse.ArgumentParser(description='Run reproducible stage-breakdown benchmark suite.')
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--tag', type=str, default='default')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--branch', type=str, default='')
    parser.add_argument('--skip-remote-sync', action='store_true')
    args = parser.parse_args()
    out = run_suite(
        build_suites()['stage_breakdown'],
        args.repeat,
        args.tag,
        dry_run=args.dry_run,
        branch=args.branch or None,
        skip_remote_sync=args.skip_remote_sync,
    )
    print(out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
