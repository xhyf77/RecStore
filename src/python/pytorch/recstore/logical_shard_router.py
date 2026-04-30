from dataclasses import dataclass
from numbers import Integral
from typing import Sequence

import torch


HASH_MOD_WORLD_SIZE = "hash_mod_world_size"
_INTEGRAL_TENSOR_DTYPES = {
    torch.uint8,
    torch.int8,
    torch.int16,
    torch.int32,
    torch.int64,
}


@dataclass(frozen=True)
class LogicalShardBucket:
    owner_rank: int
    positions: tuple[int, ...]
    fused_ids: tuple[int, ...]


def _validate_strategy(strategy: str) -> str:
    if strategy != HASH_MOD_WORLD_SIZE:
        raise ValueError(
            f"unsupported logical shard routing strategy: {strategy!r}; "
            f"expected {HASH_MOD_WORLD_SIZE!r}"
        )
    return strategy


def _validate_world_size(world_size: int) -> int:
    if not isinstance(world_size, int) or isinstance(world_size, bool) or world_size <= 0:
        raise ValueError(f"world_size must be a positive integer, got {world_size!r}")
    return world_size


def _normalize_fused_ids(fused_ids: Sequence[int] | torch.Tensor) -> list[int]:
    if isinstance(fused_ids, torch.Tensor):
        if fused_ids.dim() != 1:
            raise ValueError(f"fused_ids tensor must be 1-D, got shape {tuple(fused_ids.shape)}")
        if fused_ids.dtype not in _INTEGRAL_TENSOR_DTYPES:
            raise ValueError(
                "fused_ids tensor must use an integer dtype excluding torch.bool, "
                f"got {fused_ids.dtype}"
            )
        ids_tensor = fused_ids.to(dtype=torch.int64)
        return [int(value) for value in ids_tensor.cpu().tolist()]

    normalized_ids: list[int] = []
    for value in fused_ids:
        if isinstance(value, bool) or not isinstance(value, Integral):
            raise ValueError(
                "fused_ids sequence must contain only integer values excluding bool, "
                f"got {value!r}"
            )
        normalized_ids.append(int(value))
    return normalized_ids


def _validate_single_fused_id(fused_id: int) -> int:
    if isinstance(fused_id, bool) or not isinstance(fused_id, Integral):
        raise ValueError(
            "fused_id must be an integer value excluding bool, "
            f"got {fused_id!r}"
        )
    return int(fused_id)


def owner_rank_for_fused_id(
    fused_id: int,
    world_size: int,
    strategy: str = HASH_MOD_WORLD_SIZE,
) -> int:
    _validate_strategy(strategy)
    _validate_world_size(world_size)
    return _validate_single_fused_id(fused_id) % world_size


def owner_ranks_for_fused_ids(
    fused_ids: Sequence[int] | torch.Tensor,
    world_size: int,
    strategy: str = HASH_MOD_WORLD_SIZE,
) -> list[int]:
    ids = _normalize_fused_ids(fused_ids)
    return [
        owner_rank_for_fused_id(fused_id, world_size=world_size, strategy=strategy)
        for fused_id in ids
    ]


def bucket_fused_ids_by_owner_rank(
    fused_ids: Sequence[int] | torch.Tensor,
    world_size: int,
    strategy: str = HASH_MOD_WORLD_SIZE,
) -> list[LogicalShardBucket]:
    ids = _normalize_fused_ids(fused_ids)
    _validate_strategy(strategy)
    _validate_world_size(world_size)

    buckets_by_rank: dict[int, dict[str, list[int]]] = {}
    owner_rank_order: list[int] = []
    for position, fused_id in enumerate(ids):
        owner_rank = owner_rank_for_fused_id(
            fused_id,
            world_size=world_size,
            strategy=strategy,
        )
        if owner_rank not in buckets_by_rank:
            buckets_by_rank[owner_rank] = {"positions": [], "fused_ids": []}
            owner_rank_order.append(owner_rank)
        buckets_by_rank[owner_rank]["positions"].append(position)
        buckets_by_rank[owner_rank]["fused_ids"].append(fused_id)

    return [
        LogicalShardBucket(
            owner_rank=owner_rank,
            positions=tuple(buckets_by_rank[owner_rank]["positions"]),
            fused_ids=tuple(buckets_by_rank[owner_rank]["fused_ids"]),
        )
        for owner_rank in owner_rank_order
    ]
