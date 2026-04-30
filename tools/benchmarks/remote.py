from __future__ import annotations

import socket
import subprocess
import time
from pathlib import Path

from .common import DEFAULT_REMOTE_CONTAINER, DEFAULT_REMOTE_HOST, DEFAULT_REMOTE_REPO


def _quote_single(text: str) -> str:
    return text.replace("'", "'\"'\"'")


def ssh_run(host: str, command: str, *, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["ssh", host, command],
        check=True,
        text=True,
        capture_output=capture,
    )


def ssh_popen(host: str, command: str) -> subprocess.Popen[str]:
    return subprocess.Popen(["ssh", host, command], text=True)


def remote_docker_command(command: str, *, container: str = DEFAULT_REMOTE_CONTAINER) -> str:
    return f"docker exec {container} bash -lc '{_quote_single(command)}'"


def remote_sync_repo(*, host: str = DEFAULT_REMOTE_HOST, repo: str = DEFAULT_REMOTE_REPO, branch: str | None = None) -> None:
    parts = [f"cd {repo}"]
    if branch:
        parts.append(f"git checkout {branch}")
    parts.append("git pull --ff-only")
    ssh_run(host, " && ".join(parts))


def remote_cleanup_ps_and_paths(path_globs: list[str], *, host: str = DEFAULT_REMOTE_HOST, container: str = DEFAULT_REMOTE_CONTAINER) -> None:
    script = [
        "set -e",
        "for p in $(ps -eo pid,args | awk '/\\/build\\/bin\\/ps_server/ && !/awk/ {print $1}'); do kill -9 \"$p\" || true; done",
    ]
    if path_globs:
        script.append("rm -rf " + " ".join(path_globs))
    script.append("echo cleaned")
    ssh_run(host, remote_docker_command("; ".join(script), container=container))


def remote_start_ps(config_path: str, *, host: str = DEFAULT_REMOTE_HOST, container: str = DEFAULT_REMOTE_CONTAINER) -> subprocess.Popen[str]:
    command = remote_docker_command(f"cd /app/RecStore && ./build/bin/ps_server --config_path {config_path}", container=container)
    return ssh_popen(host, command)


def remote_run_worker(command: str, *, host: str = DEFAULT_REMOTE_HOST, container: str = DEFAULT_REMOTE_CONTAINER) -> subprocess.Popen[str]:
    return ssh_popen(host, remote_docker_command(command, container=container))


def stop_process(proc: subprocess.Popen[str] | None) -> None:
    if proc is None:
        return
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def wait_remote_ports(host: str, ports: list[int], timeout_s: float = 20.0) -> None:
    deadline = time.time() + timeout_s
    remaining = set(ports)
    while time.time() < deadline and remaining:
        done: list[int] = []
        for port in sorted(remaining):
            sock = socket.socket()
            sock.settimeout(0.5)
            try:
                sock.connect((host, port))
                done.append(port)
            except OSError:
                pass
            finally:
                sock.close()
        for port in done:
            remaining.discard(port)
        if remaining:
            time.sleep(0.2)
    if remaining:
        raise RuntimeError(f"remote ports not ready on {host}: {sorted(remaining)}")
