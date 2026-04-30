import unittest

import torch

from src.python.pytorch.recstore.logical_shard_router import (
    HASH_MOD_WORLD_SIZE,
    bucket_fused_ids_by_owner_rank,
    owner_rank_for_fused_id,
    owner_ranks_for_fused_ids,
)


class TestLogicalShardRouter(unittest.TestCase):
    def test_owner_rank_mapping_is_stable(self):
        fused_ids = torch.tensor([0, 1, 7, 8, 13, 21], dtype=torch.int64)

        owner_ranks = owner_ranks_for_fused_ids(fused_ids, world_size=4)

        self.assertEqual(owner_ranks, [0, 1, 3, 0, 1, 1])
        self.assertEqual(
            owner_ranks,
            owner_ranks_for_fused_ids(fused_ids, world_size=4, strategy=HASH_MOD_WORLD_SIZE),
        )
        self.assertEqual(owner_rank_for_fused_id(21, world_size=4), 1)

    def test_empty_input_returns_empty_buckets_and_ranks(self):
        self.assertEqual(owner_ranks_for_fused_ids([], world_size=2), [])
        self.assertEqual(bucket_fused_ids_by_owner_rank([], world_size=2), [])

    def test_duplicate_ids_are_bucketed_without_dedup(self):
        fused_ids = [5, 2, 5, 6, 2]

        buckets = bucket_fused_ids_by_owner_rank(fused_ids, world_size=3)

        self.assertEqual(len(buckets), 2)
        self.assertEqual(buckets[0].owner_rank, 2)
        self.assertEqual(buckets[0].positions, (0, 1, 2, 4))
        self.assertEqual(buckets[0].fused_ids, (5, 2, 5, 2))
        self.assertEqual(buckets[1].owner_rank, 6 % 3)
        self.assertEqual(buckets[1].positions, (3,))
        self.assertEqual(buckets[1].fused_ids, (6,))

    def test_bucket_positions_can_reconstruct_original_order(self):
        fused_ids = torch.tensor([10, 3, 4, 11, 6, 7], dtype=torch.int64)

        buckets = bucket_fused_ids_by_owner_rank(fused_ids, world_size=4)

        rebuilt = [None] * fused_ids.numel()
        for bucket in buckets:
            for position, fused_id in zip(bucket.positions, bucket.fused_ids):
                rebuilt[position] = fused_id

        self.assertEqual(rebuilt, fused_ids.tolist())

    def test_invalid_world_size_raises_clear_error(self):
        for world_size in (0, -1, True):
            with self.assertRaisesRegex(ValueError, "world_size must be a positive integer"):
                owner_rank_for_fused_id(1, world_size=world_size)

    def test_invalid_strategy_raises_clear_error(self):
        with self.assertRaisesRegex(
            ValueError,
            "unsupported logical shard routing strategy",
        ):
            bucket_fused_ids_by_owner_rank([1, 2, 3], world_size=2, strategy="cityhash")

    def test_non_integral_fused_ids_raise_clear_error(self):
        with self.assertRaisesRegex(
            ValueError,
            "fused_id must be an integer value excluding bool",
        ):
            owner_rank_for_fused_id(1.5, world_size=2)

        with self.assertRaisesRegex(
            ValueError,
            "fused_id must be an integer value excluding bool",
        ):
            owner_rank_for_fused_id(True, world_size=2)

        with self.assertRaisesRegex(
            ValueError,
            "fused_ids tensor must use an integer dtype excluding torch.bool",
        ):
            owner_ranks_for_fused_ids(torch.tensor([1.0, 2.5], dtype=torch.float32), world_size=2)

        with self.assertRaisesRegex(
            ValueError,
            "fused_ids tensor must use an integer dtype excluding torch.bool",
        ):
            owner_ranks_for_fused_ids(torch.tensor([True, False], dtype=torch.bool), world_size=2)

        with self.assertRaisesRegex(
            ValueError,
            "fused_ids sequence must contain only integer values excluding bool",
        ):
            bucket_fused_ids_by_owner_rank([1, 2.5, 3], world_size=2)

        with self.assertRaisesRegex(
            ValueError,
            "fused_ids sequence must contain only integer values excluding bool",
        ):
            bucket_fused_ids_by_owner_rank([1, True, 3], world_size=2)


if __name__ == "__main__":
    unittest.main()
