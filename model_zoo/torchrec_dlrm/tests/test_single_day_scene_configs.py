import unittest
from pathlib import Path

from model_zoo.torchrec_dlrm.launch_config import build_config_from_sources


class SingleDaySceneConfigTest(unittest.TestCase):
    def test_recstore_random_65536_config(self) -> None:
        config_path = (
            Path(__file__).resolve().parents[1]
            / "configs"
            / "single_day_recstore_random_65536.gin"
        )

        config = build_config_from_sources(
            gin_config=str(config_path),
            gin_bindings=[],
            cli_overrides={},
        )

        self.assertFalse(config.use_torchrec)
        self.assertTrue(config.use_random_dataset)
        self.assertEqual(config.dataset_size, 65536)
        self.assertEqual(config.batch_size, 1024)
        self.assertTrue(config.enable_prefetch)
        self.assertEqual(config.prefetch_depth, 2)


if __name__ == "__main__":
    unittest.main()
