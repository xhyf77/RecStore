import unittest

from ps_server_runner import PSServerRunner


class TestPSServerRunner(unittest.TestCase):
    def test_suppresses_following_stack_frames_after_sigterm_marker(self):
        runner = PSServerRunner(verbose=False)
        runner._stopping = True

        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "*** Signal 15 (SIGTERM) received by PID 123"
            )
        )
        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "    @ 00000000000dc2c6 std::thread::join()"
            )
        )
        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "                       /app/RecStore/src/ps/ps_server.cpp:128"
            )
        )

    def test_suppresses_expected_sigterm_shutdown_noise(self):
        runner = PSServerRunner(verbose=False)
        runner._stopping = True

        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "*** Signal 15 (SIGTERM) received by PID 123"
            )
        )
        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "*** Aborted at 1777209466 (Unix time)"
            )
        )
        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "    @ 0000000000e24681 folly::symbolizer::signalHandler"
            )
        )

    def test_keeps_regular_stderr_lines(self):
        runner = PSServerRunner(verbose=False)
        runner._stopping = True

        self.assertFalse(
            runner._should_suppress_shutdown_stderr(
                "FATAL: Failed to start gRPC server shard 0"
            )
        )
        self.assertFalse(
            runner._should_suppress_shutdown_stderr(
                "runtime error: unexpected shutdown path"
            )
        )

    def test_regular_stderr_after_suppressed_block_is_kept(self):
        runner = PSServerRunner(verbose=False)
        runner._stopping = True

        self.assertTrue(
            runner._should_suppress_shutdown_stderr(
                "*** Aborted at 1777209466 (Unix time)"
            )
        )
        self.assertFalse(
            runner._should_suppress_shutdown_stderr(
                "FATAL: Failed to start gRPC server shard 0"
            )
        )


if __name__ == "__main__":
    unittest.main()
