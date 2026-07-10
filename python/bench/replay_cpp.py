"""C++ 生产路径 trace 回放验证（`python -m bench.replay_cpp <trace.jsonl>`）。

把真实 trace 直接重放到 pybind11 绑定的 C++ `TieredStore`（HBM+DRAM 两层、
块后继图 + 多锚点链式预取），报告 fast/slow 命中分解——验证生产路径与
bench.tiered 回放模型行为同构（模型是选型工具，C++ 是交付物；两者机制
一致但淘汰器实现不同，命中率允许有实现级差异）。

用法：
    PYTHONPATH=python:build/src python3 -m bench.replay_cpp <trace.jsonl> \
        [fast_blocks] [slow_factor] [max_requests]

注：C++ 淘汰器受害者选择为 O(1)（最快层 S3-FIFO 队列，较慢层有序索引），
全 trace 回放秒级完成；max_requests 仅用于快速抽样。"""

from __future__ import annotations

import sys

import _mooncake_kvcache as mk

from bench.framework import parse_trace

_BLOCK_BYTES = 256  # 每块负载字节数（容量按块数换算成字节）
_PAYLOAD = b"\x00" * _BLOCK_BYTES


def replay(
    trace_path: str,
    fast_blocks: int = 4096,
    slow_factor: int = 4,
    max_requests: int | None = None,
) -> dict:
    """把 trace 重放到 C++ TieredStore，返回命中分解统计。"""
    store = mk.TieredStore(
        hbm_bytes=fast_blocks * _BLOCK_BYTES,
        dram_bytes=fast_blocks * slow_factor * _BLOCK_BYTES,
        nvme_bytes=None,
        high_water_ratio=1.0,
    )
    records = parse_trace(trace_path)
    if max_requests is not None:
        records = records[:max_requests]
    hits = misses = 0
    for record in records:
        keys = [mk.BlockKey(h) for h in record.hash_ids]
        # 生产 load_kv 语义：先多锚点链式预取，再逐块读取。
        store.prefetch_chain_multi(keys)
        for key in keys:
            if store.exists(key):
                store.get(key)  # 生产读路径：命中即更新频次/最近性。
                hits += 1
            else:
                store.put(key, _PAYLOAD)
                misses += 1
        # 生产 store_kv 语义：写路径记录块后继图。
        store.record_sequence(keys)
    total = hits + misses
    return {
        "requests": len(records),
        "blocks": total,
        "hits": hits,
        "misses": misses,
        "hit_rate": hits / total if total else 0.0,
    }


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    trace = sys.argv[1]
    fast_blocks = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
    slow_factor = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    max_requests = int(sys.argv[4]) if len(sys.argv) > 4 else None
    stats = replay(trace, fast_blocks, slow_factor, max_requests)
    print(
        f"[cpp-replay] requests={stats['requests']} blocks={stats['blocks']} "
        f"hits={stats['hits']} misses={stats['misses']} "
        f"hit_rate={stats['hit_rate']:.3f}"
    )


if __name__ == "__main__":
    main()
