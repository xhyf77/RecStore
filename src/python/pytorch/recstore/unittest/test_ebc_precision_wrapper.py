import unittest
import os
import sys
import argparse
import json
import torch
import importlib.util
import subprocess
from importlib.util import find_spec

SRC_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../../../'))

if SRC_ROOT not in sys.path:
    sys.path.insert(0, SRC_ROOT)

TEST_SCRIPTS_PATH = os.path.join(SRC_ROOT, 'test/scripts')
if TEST_SCRIPTS_PATH not in sys.path:
    sys.path.insert(0, TEST_SCRIPTS_PATH)

from ps_server_runner import ps_server_context
from ps_server_helpers import should_skip_server_start, get_server_config

TEST_MODULE_PATH = os.path.join(os.path.dirname(__file__), 'test_ebc_precision.py')
MP_TEST_MODULE_PATH = os.path.join(os.path.dirname(__file__), 'test_ebc_precision_multiprocess.py')

_server_runner = None
_test_result = None


def _resolve_repo_config_path():
    config_path = os.path.join(REPO_ROOT, 'recstore_config.json')
    if not os.path.exists(config_path):
        raise FileNotFoundError(f"recstore_config.json not found at {config_path}")
    return config_path


def _resolve_ps_endpoint(config_path=None):
    if config_path is None:
        config_path = _resolve_repo_config_path()

    host = "127.0.0.1"
    port = 15000
    with open(config_path, 'r') as f:
        cdata = json.load(f)

    found = False
    if "client" in cdata:
        if "port" in cdata["client"]:
            port = cdata["client"]["port"]
            found = True
        if "host" in cdata["client"]:
            host = cdata["client"]["host"]

    if not found and "cache_ps" in cdata and "servers" in cdata["cache_ps"]:
        servers = cdata["cache_ps"]["servers"]
        if isinstance(servers, list) and len(servers) > 0:
            p = servers[0].get("port")
            h = servers[0].get("host")
            if p is not None:
                port = p
            if h is not None:
                host = h

    return host, port


def _has_torchrec():
    return find_spec("torchrec") is not None


def _require_torchrec(testcase):
    if not _has_torchrec():
        testcase.skipTest("torchrec is not installed in this test environment")

def _lazy_import_test_module():
    """Lazy import test_ebc_precision to avoid module-level torchrec imports"""
    spec = importlib.util.spec_from_file_location("test_ebc_precision_module", TEST_MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def setUpModule():
    global _server_runner
    
    print(f"\n{'='*70}")
    print("SETUP MODULE: Initializing EBC Precision Test Suite")
    print(f"{'='*70}\n")
    
    try:
        skip_server, reason = should_skip_server_start()
        if skip_server:
            print(f"[{reason}] Running tests assuming ps_server is already running")
            print(f"  - Verifying PS Server connectivity...")
            
            # In CI, server should already be running. Verify connectivity.
            from ps_server_helpers import check_ps_server_running
            is_running, open_ports = check_ps_server_running()
            if is_running:
                print(f"  ✓ PS Server verified running on ports: {open_ports}\n")
            else:
                print(f"  ⚠️  Warning: PS Server ports not responding, but continuing anyway")
                print(f"     Tests may fail if server is not actually running\n")
            return
        
        config = get_server_config()
        
        print(f"Starting PS Server for EBC Precision Tests")
        print(f"  Server path: {config['server_path']}")
        print(f"  Config: {config['config_path'] or 'default'}")
        print(f"  Log dir: {config['log_dir']}")
        print(f"  Timeout: {config['timeout']}s\n")
        
        from ps_server_runner import PSServerRunner
        _server_runner = PSServerRunner(
            server_path=config['server_path'],
            config_path=config['config_path'],
            log_dir=config['log_dir'],
            timeout=config['timeout'],
            num_shards=config['num_shards'],
            verbose=True
        )
        
        if not _server_runner.start():
            raise RuntimeError("Failed to start PS Server")
        
        # Wait for server to be fully ready
        import time
        print("Waiting for PS Server to be fully ready...")
        time.sleep(2)  # Give server extra time to initialize
        print("✓ PS Server ready for tests\n")
    except Exception as e:
        print(f"\n❌ setUpModule failed: {e}")
        import traceback
        traceback.print_exc()
        raise


def tearDownModule():
    global _server_runner
    
    if _server_runner is None:
        print("\nNo server runner to clean up")
        return
    
    try:
        print(f"\n{'='*70}")
        print("Stopping PS Server")
        print(f"{'='*70}\n")
        
        if _server_runner.is_running():
            if not _server_runner.stop():
                print("⚠️ Server stop returned False, but continuing...")
        
        print("✅ PS Server stopped gracefully\n")
    except Exception as e:
        print(f"\n⚠️ tearDownModule exception (non-fatal): {e}")
        import traceback
        traceback.print_exc()
        # Do NOT raise - we want tests to pass even if cleanup has issues
    finally:
        _server_runner = None


class TestEBCPrecision(unittest.TestCase):
    def test_basic_precision_cpu(self):
        _require_torchrec(self)
        print("\n" + "="*70)
        print("Running Basic EBC Precision Test (CPU)")
        print("="*70)
        
        args = argparse.Namespace(
            num_embeddings=1000,
            embedding_dim=128,  # Backend fixed to 128
            batch_size=64,
            seed=42,
            cpu=True
        )
        
        try:
            # Lazy import to avoid module-level torchrec loading
            test_ebc_precision = _lazy_import_test_module()
            print(f"✓ Successfully imported test_ebc_precision module")
            
            # Call the standalone test main function
            print(f"Starting precision test execution...")
            test_ebc_precision.main(args)
            print("\n✅ Basic precision test completed successfully")
        except SystemExit as e:
            # Segfault often manifests as SystemExit with code 139
            if e.code == 139 or e.code == -11:
                self.fail(f"Basic precision test crashed with exit code {e.code} (likely segfault)")
            else:
                self.fail(f"Basic precision test exited with code {e.code}")
        except AssertionError as e:
            self.fail(f"Basic precision test failed: {e}")
        except Exception as e:
            self.fail(f"Basic precision test raised unexpected exception: {type(e).__name__}: {e}")
            import traceback
            traceback.print_exc()
    
    def test_small_batch_precision(self):
        _require_torchrec(self)
        print("\n" + "="*70)
        print("Running Small Batch EBC Precision Test (CPU)")
        print("="*70)
        
        args = argparse.Namespace(
            num_embeddings=500,
            embedding_dim=128,
            batch_size=16,
            seed=42,
            cpu=True
        )
        
        try:
            # Lazy import to avoid module-level torchrec loading
            test_ebc_precision = _lazy_import_test_module()
            test_ebc_precision.main(args)
            print("\n✅ Small batch precision test completed successfully")
        except SystemExit as e:
            if e.code == 139 or e.code == -11:
                self.fail(f"Small batch precision test crashed with exit code {e.code} (likely segfault)")
            else:
                self.fail(f"Small batch precision test exited with code {e.code}")
        except AssertionError as e:
            self.fail(f"Small batch precision test failed: {e}")
        except Exception as e:
            self.fail(f"Small batch precision test raised unexpected exception: {type(e).__name__}: {e}")
    
    @unittest.skipIf(not torch.cuda.is_available(), "CUDA not available")
    def test_cuda_precision(self):
        _require_torchrec(self)
        print("\n" + "="*70)
        print("Running CUDA EBC Precision Test")
        print("="*70)
        
        args = argparse.Namespace(
            num_embeddings=1000,
            embedding_dim=128,
            batch_size=64,
            seed=42,
            cpu=False
        )
        
        try:
            # Lazy import to avoid module-level torchrec loading
            test_ebc_precision = _lazy_import_test_module()
            test_ebc_precision.main(args)
            print("\n✅ CUDA precision test completed successfully")
        except ImportError as e:
            self.skipTest(f"CUDA test skipped due to import error (likely FBGEMM): {e}")
        except SystemExit as e:
            if e.code == 139 or e.code == -11:
                self.fail(f"CUDA precision test crashed with exit code {e.code} (likely segfault)")
            else:
                self.fail(f"CUDA precision test exited with code {e.code}")
        except AssertionError as e:
            self.fail(f"CUDA precision test failed: {e}")
        except Exception as e:
            self.fail(f"CUDA precision test raised unexpected exception: {type(e).__name__}: {e}")

    def test_multiprocess_precision(self):
        _require_torchrec(self)
        print("\n" + "="*70)
        print("Running Multiprocess EBC Precision Test (Subprocess)")
        print("="*70)
        
        # We run this as a subprocess to avoid multiprocessing context/pickling issues
        # when running under unittest discovery.
        cmd = [
            sys.executable, 
            MP_TEST_MODULE_PATH,
            "--num-embeddings", "1000",
            "--embedding-dim", "128",
            "--batch-size", "32",
            "--world-size", "2",
            "--cpu",
            "--seed", "42"
        ]

        try:
            config_path = _resolve_repo_config_path()
            if _server_runner and _server_runner.config_path:
                config_path = str(_server_runner.config_path)

            host, port = _resolve_ps_endpoint(config_path)
            cmd.extend(["--ps-host", str(host), "--ps-port", str(port)])
            print(f"  Passed PS Config: {host}:{port}")

        except Exception as e:
            print(f"  Warning: Could not determine specific PS config: {e}")

        
        print(f"Executing command: {' '.join(cmd)}")
        
        try:
            # check_call raises CalledProcessError if return code != 0
            subprocess.check_call(cmd)
            print("\n✅ Multiprocess precision test completed successfully")
        except subprocess.CalledProcessError as e:
            self.fail(f"Multiprocess precision test failed with exit code {e.returncode}")
        except Exception as e:
            self.fail(f"Multiprocess precision test raised unexpected exception: {e}")
