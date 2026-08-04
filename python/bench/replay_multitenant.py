"""多租户配额隔离回放（`python -m bench.replay_multitenant <trace_a> <trace_b>`）。

把两条真实 trace 按到达顺序交错为两个租户共享同一 C++ TieredStore：
租户 A 为受保护业务，租户 B 为洪水式邻居（如批量扫描负载）。对比
无配额与多组权重（50/50、75/25、90/10）下各租户命中率，验证加权
配额的 SLO 分级能力（加权 max-min：超额租户优先受害，空闲份额仍可借用）。

用法：
    PYTHONPATH=python:build/src python3 -m bench.replay_multitenant \
        <trace_a.jsonl> <trace_b.jsonl|FLOOD> [fast_blocks] [slow_factor]

trace_b 传 "FLOOD" 时生成对抗性洪水租户：每请求 32 个从不复用的唯一块
（纯扫描负载，最能污染较慢层 LRU）。"""

from __future__ import annotations

import sys

import _mooncake_kvcache as mk

from bench.framework import TraceRecord, parse_trace

_BLOCK_BYTES = 256
_PAYLOAD = b"\x00" * _BLOCK_BYTES
# 租户键空间隔离：不同租户的同名 hash_id 不应互相命中。
_TENANT_SALT = 1 << 48


def replay_pair(
    trace_a: str,
    trace_b: str,
    fast_blocks: int = 4096,
    slow_factor: int = 4,
    weights: dict[int, float] | None = None,
) -> dict:
    """交错回放两租户，返回各租户命中率。weights 为 None/空即无配额。"""
    weights = weights or {}
    store = mk.TieredStore(
        hbm_bytes=fast_blocks * _BLOCK_BYTES,
        dram_bytes=fast_blocks * slow_factor * _BLOCK_BYTES,
        nvme_bytes=None,
        high_water_ratio=1.0,
        tenant_weights=weights,
    )
    records_a = parse_trace(trace_a)
    if trace_b == "FLOOD":
        records_b = _flood_records(len(records_a))
    else:
        records_b = parse_trace(trace_b)
    streams = {1: records_a, 2: records_b}
    hits = {1: 0, 2: 0}
    total = {1: 0, 2: 0}
    # 逐请求轮转交错（保持各自到达顺序），模拟并发共享存储。
    max_len = max(len(records) for records in streams.values())
    for i in range(max_len):
        for tenant, records in streams.items():
            if i >= len(records):
                continue
            keys = [
                mk.BlockKey(h + tenant * _TENANT_SALT)
                for h in records[i].hash_ids
            ]
            store.prefetch_chain_multi(keys)
            for key in keys:
                total[tenant] += 1
                if store.exists(key):
                    store.get(key)
                    hits[tenant] += 1
                else:
                    store.put(key, _PAYLOAD, tenant_id=tenant)
            store.record_sequence(keys)
    return {
        tenant: hits[tenant] / total[tenant] if total[tenant] else 0.0
        for tenant in streams
    }


def _flood_records(count: int, blocks_per_request: int = 32) -> list[TraceRecord]:
    """对抗性洪水租户：全部块唯一、零复用（纯扫描）。"""
    records = []
    next_id = 1
    for i in range(count):
        ids = tuple(range(next_id, next_id + blocks_per_request))
        next_id += blocks_per_request
        records.append(TraceRecord(i, blocks_per_request * 512, 1, ids))
    return records


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    trace_a, trace_b = sys.argv[1], sys.argv[2]
    fast_blocks = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
    slow_factor = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    for tag, weights in (
        ("无配额  ", None),
        ("配额50/50", {1: 0.5, 2: 0.5}),
        ("配额75/25", {1: 0.75, 2: 0.25}),
        ("配额90/10", {1: 0.9, 2: 0.1}),
    ):
        rates = replay_pair(trace_a, trace_b, fast_blocks, slow_factor, weights)
        print(
            f"[multitenant {tag}] tenant_A_hit={rates[1]:.3f} "
            f"tenant_B_hit={rates[2]:.3f}"
        )


if __name__ == "__main__":
    main()
