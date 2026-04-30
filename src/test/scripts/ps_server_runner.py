#!/usr/bin/env python3

import os
import sys
import time
import signal
import subprocess
import threading
import argparse
from pathlib import Path
from datetime import datetime
from contextlib import contextmanager
from typing import Optional, List, Callable


class PSServerRunner:
    def __init__(
        self,
        server_path: str = "./build/bin/ps_server",
        config_path: Optional[str] = None,
        log_dir: str = "/tmp",
        timeout: int = 120,
        num_shards: int = 2,
        verbose: bool = False,
        startup_delay: float = 2.0
    ):
        self.server_path = Path(server_path)
        self.config_path = Path(config_path) if config_path else None
        self.log_dir = Path(log_dir)
        self.timeout = timeout
        self.num_shards = num_shards
        self.verbose = verbose
        self.startup_delay = startup_delay
        
        self.process: Optional[subprocess.Popen] = None
        self.log_file = None
        self.stdout_thread = None
        self.stderr_thread = None
        self.ready_event = threading.Event()
        self.shard_ready = set()
        self._stopping = False
        self._suppressing_shutdown_stderr = False
        
        self.log_dir.mkdir(parents=True, exist_ok=True)
        
    def _create_log_file(self):
        import getpass
        try:
            user = getpass.getuser()
        except:
            user = "unknown"
            
        script_name = os.path.basename(sys.argv[0])
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        
        log_filename = f"ps_server_{user}_{script_name}_{timestamp}.log"
        log_path = self.log_dir / log_filename
        
        self.log_file = open(log_path, 'w', buffering=1)
        return log_path
    
    def _log(self, message: str):
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        log_line = f"[{timestamp}] {message}\n"
        if self.log_file:
            self.log_file.write(log_line)
        if self.verbose:
            print(log_line.rstrip())

    def _should_suppress_shutdown_stderr(self, line: str) -> bool:
        if not self._stopping:
            self._suppressing_shutdown_stderr = False
            return False

        block_start_markers = (
            "*** Aborted at ",
            "*** Signal 15 (SIGTERM)",
            "folly::symbolizer::",
        )
        if any(marker in line for marker in block_start_markers):
            self._suppressing_shutdown_stderr = True
            return True

        if self._suppressing_shutdown_stderr:
            stack_frame_prefixes = (
                "    @ ",
                "\t@ ",
                "                       /",
                "/app/RecStore/",
            )
            if line.startswith(stack_frame_prefixes):
                return True
            self._suppressing_shutdown_stderr = False

        return False
    
    def _monitor_output(self, pipe, stream_name: str):
        try:
            for line in iter(pipe.readline, ''):
                if not line:
                    break
                line = line.rstrip()

                if stream_name == "STDERR" and self._should_suppress_shutdown_stderr(line):
                    continue

                self._log(f"[{stream_name}] {line}")
                
                if "listening on" in line:
                    try:
                        if "Server shard" in line:
                            shard_id = int(line.split("shard")[1].split()[0])
                        elif "Server listening on" in line:
                            shard_id = 0 # Default to shard 0 for single server mode
                        else:
                            continue
                            
                        self.shard_ready.add(shard_id)
                        self._log(f"✓ Detected shard {shard_id} ready")
                        
                        if len(self.shard_ready) >= self.num_shards:
                            self._log(f"✓ All {self.num_shards} shards ready")
                            self.ready_event.set()
                    except (IndexError, ValueError) as e:
                        self._log(f"⚠ Failed to parse shard info: {e}")
        except Exception as e:
            self._log(f"✗ Monitor thread error: {e}")
        finally:
            pipe.close()
    
    def start(self) -> bool:
        if not self.server_path.exists():
            raise FileNotFoundError(f"Server binary not found: {self.server_path}")
        
        log_path = self._create_log_file()
        self._log(f"Starting PS Server: {self.server_path}")
        self._log(f"Log file: {log_path}")
        
        cmd = [str(self.server_path)]
        if self.config_path:
            cmd.extend(["--config_path", str(self.config_path)])
            self._log(f"Config: {self.config_path}")
        
        try:
            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
                cwd=str(self.server_path.parent.parent.parent)
            )
            
            self._log(f"Server process started (PID: {self.process.pid})")
            
            self.stdout_thread = threading.Thread(
                target=self._monitor_output,
                args=(self.process.stdout, "STDOUT"),
                daemon=True
            )
            self.stderr_thread = threading.Thread(
                target=self._monitor_output,
                args=(self.process.stderr, "STDERR"),
                daemon=True
            )
            
            self.stdout_thread.start()
            self.stderr_thread.start()
            
            time.sleep(self.startup_delay)
            
            self._log(f"Waiting for {self.num_shards} shards to be ready (timeout: {self.timeout}s)...")
            
            start_time = time.time()
            check_interval = 0.5
            
            while not self.ready_event.is_set():
                if time.time() - start_time > self.timeout:
                    self._log(f"✗ Timeout waiting for server to be ready")
                    self._log(f"   Detected {len(self.shard_ready)}/{self.num_shards} shards: {sorted(self.shard_ready)}")
                    self.stop()
                    return False
                
                if self.process.poll() is not None:
                    self._log(f"✗ Server process terminated unexpectedly (exit code: {self.process.returncode})")
                    self.stop()
                    return False
                
                time.sleep(check_interval)
            
            time.sleep(0.5)
            
            self._log("✓ Server is ready")
            return True
                
        except Exception as e:
            self._log(f"✗ Failed to start server: {e}")
            self.stop()
            raise
    
    def stop(self):
        if self.process is None:
            return
        
        self._log("Stopping PS Server...")
        self._stopping = True
        self._suppressing_shutdown_stderr = False
        
        try:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                    self._log("✓ Server terminated gracefully")
                except subprocess.TimeoutExpired:
                    self._log("⚠ Server didn't terminate, killing...")
                    self.process.kill()
                    self.process.wait()
                    self._log("✓ Server killed")
        except Exception as e:
            self._log(f"✗ Error stopping server: {e}")
        
        if self.stdout_thread:
            self.stdout_thread.join(timeout=2)
        if self.stderr_thread:
            self.stderr_thread.join(timeout=2)
        
        if self.log_file:
            self.log_file.close()
            self._log = lambda msg: None
        
        self.process = None
        self._stopping = False
        self._suppressing_shutdown_stderr = False
    
    def is_running(self) -> bool:
        return self.process is not None and self.process.poll() is None
    
    @contextmanager
    def run(self):
        success = self.start()
        if not success:
            raise RuntimeError("Failed to start PS Server")
        
        try:
            yield self
        finally:
            self.stop()


@contextmanager
def ps_server_context(
    server_path: str = "./build/bin/ps_server",
    config_path: Optional[str] = None,
    log_dir: str = "/tmp",
    timeout: int = 120,
    num_shards: int = 2,
    verbose: bool = False,
    startup_delay: float = 2.0
):
    runner = PSServerRunner(
        server_path=server_path,
        config_path=config_path,
        log_dir=log_dir,
        timeout=timeout,
        num_shards=num_shards,
        verbose=verbose,
        startup_delay=startup_delay
    )
    
    with runner.run():
        yield runner


def run_with_server(
    test_func: Callable,
    server_path: str = "./build/bin/ps_server",
    config_path: Optional[str] = None,
    log_dir: str = "/tmp",
    timeout: int = 30,
    num_shards: int = 2,
    verbose: bool = False
):
    with ps_server_context(
        server_path=server_path,
        config_path=config_path,
        log_dir=log_dir,
        timeout=timeout,
        num_shards=num_shards,
        verbose=verbose
    ):
        return test_func()


def main():
    parser = argparse.ArgumentParser(
        description="PS Server Test Runner - Manages ps_server lifecycle for tests",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Start server and keep it running
  python3 ps_server_runner.py --keep-alive
  
  # Run a test command with server
  python3 ps_server_runner.py --command "python3 test.py"
  
  # Use custom config and log directory
  python3 ps_server_runner.py --config recstore_config.json --log-dir ./test_logs --keep-alive
  
  # In Python code:
  from ps_server_runner import ps_server_context
  
  with ps_server_context():
      # Run your tests here
      run_tests()
        """
    )
    
    parser.add_argument(
        "--server-path",
        default="./build/bin/ps_server",
        help="Path to ps_server binary (default: ./build/bin/ps_server)"
    )
    parser.add_argument(
        "--config",
        help="Path to recstore config file"
    )
    parser.add_argument(
        "--log-dir",
        default="/tmp",
        help="Directory for server logs (default: /tmp)"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=30,
        help="Timeout in seconds waiting for server ready (default: 30)"
    )
    parser.add_argument(
        "--num-shards",
        type=int,
        default=2,
        help="Expected number of server shards (default: 2)"
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print server output to console"
    )
    parser.add_argument(
        "--keep-alive",
        action="store_true",
        help="Keep server running (for interactive testing)"
    )
    parser.add_argument(
        "--command",
        help="Command to run after server starts"
    )
    
    args = parser.parse_args()
    
    runner = PSServerRunner(
        server_path=args.server_path,
        config_path=args.config,
        log_dir=args.log_dir,
        timeout=args.timeout,
        num_shards=args.num_shards,
        verbose=args.verbose or args.keep_alive
    )
    
    try:
        if not runner.start():
            print("✗ Failed to start server", file=sys.stderr)
            return 1
        
        if args.command:
            print(f"\n{'='*70}")
            print(f"Running command: {args.command}")
            print(f"{'='*70}\n")
            
            result = subprocess.run(args.command, shell=True)
            exit_code = result.returncode
            
            print(f"\n{'='*70}")
            print(f"Command exited with code: {exit_code}")
            print(f"{'='*70}\n")
            
            return exit_code
        
        elif args.keep_alive:
            print(f"\n{'='*70}")
            print("Server is running. Press Ctrl+C to stop.")
            print(f"{'='*70}\n")
            
            try:
                while runner.is_running():
                    time.sleep(1)
            except KeyboardInterrupt:
                print("\n\nReceived interrupt, stopping server...")
        
        return 0
        
    except Exception as e:
        print(f"✗ Error: {e}", file=sys.stderr)
        return 1
    
    finally:
        runner.stop()


if __name__ == "__main__":
    sys.exit(main())
