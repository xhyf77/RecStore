import pathlib
import unittest


class RunSingleDayScriptTest(unittest.TestCase):
    def test_uses_standalone_only_when_local_ps_is_requested(self) -> None:
        script_path = pathlib.Path(__file__).resolve().parents[1] / "run_single_day.sh"
        script_text = script_path.read_text(encoding="utf-8")

        self.assertIn('if [ "$start_ps" = true ]; then', script_text)
        self.assertIn("launcher_args+=(--standalone)", script_text)
        self.assertIn('launcher_args+=(--rdzv_backend c10d --rdzv_endpoint localhost --rdzv_id "run-$(date +%s)" --role trainer)', script_text)


if __name__ == "__main__":
    unittest.main()
