import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ps_test_config import (
    DEFAULT_BRPC_BENCHMARK_CONFIG,
    DEFAULT_GRPC_MAIN_CONFIG,
    DEFAULT_RDMA_MULTI_SHARD_CONFIG,
    DEFAULT_RDMA_SINGLE_SHARD_CONFIG,
    load_client_endpoint,
    resolve_rdma_integration_config,
)


class TestPSTestConfig(unittest.TestCase):
    def test_rdma_config_roles_are_explicit(self):
        self.assertEqual(
            DEFAULT_RDMA_SINGLE_SHARD_CONFIG,
            "./src/test/scripts/recstore_config.rdma_test.json",
        )
        self.assertEqual(
            DEFAULT_RDMA_MULTI_SHARD_CONFIG,
            "./src/test/scripts/recstore_config.rdma_multishard_test.json",
        )

    def test_default_non_rdma_config_roles_are_explicit(self):
        self.assertEqual(DEFAULT_GRPC_MAIN_CONFIG, "./recstore_config.json")
        self.assertEqual(
            DEFAULT_BRPC_BENCHMARK_CONFIG,
            "./src/test/scripts/recstore_config.brpc.json",
        )

    def test_resolve_rdma_integration_config_prefers_explicit_path(self):
        self.assertEqual(
            resolve_rdma_integration_config(server_count=2, config_path="./custom.json"),
            "./custom.json",
        )

    def test_resolve_rdma_integration_config_uses_single_shard_default(self):
        self.assertEqual(
            resolve_rdma_integration_config(server_count=1, config_path=None),
            DEFAULT_RDMA_SINGLE_SHARD_CONFIG,
        )

    def test_resolve_rdma_integration_config_uses_multi_shard_default(self):
        self.assertEqual(
            resolve_rdma_integration_config(server_count=2, config_path=None),
            DEFAULT_RDMA_MULTI_SHARD_CONFIG,
        )

    def test_load_client_endpoint_for_default_grpc_config(self):
        host, port = load_client_endpoint(DEFAULT_GRPC_MAIN_CONFIG)
        self.assertEqual(host, "127.0.0.1")
        self.assertEqual(port, 15000)

    def test_load_client_endpoint_for_brpc_benchmark_config(self):
        host, port = load_client_endpoint(DEFAULT_BRPC_BENCHMARK_CONFIG)
        self.assertEqual(host, "127.0.0.1")
        self.assertEqual(port, 25000)


if __name__ == "__main__":
    unittest.main()
