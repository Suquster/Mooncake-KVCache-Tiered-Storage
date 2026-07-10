"""两级（fast/slow）分层回放模拟 + 前缀感知预取。

对应 C++ 侧 HBM▸DRAM 分层与 `TieredStorageManager::Prefetch`（需求 2.6）：
  * fast 层命中：零额外开销（KVCache 已在 HBM）。
  * slow 层命中：付出层间搬运开销后晋升到 fast 层（免 prefill 重算）。
  * 未命中：付出完整 prefill 开销后准入 fast 层。
  * fast 层淘汰的块降级到 slow 层（on_evict 回调接管），slow 层淘汰即丢弃。

前缀感知预取：维护块后继图（同一请求内 hash_ids 相邻关系）。请求到达时先看
首块：若命中则沿后继链预测本请求将顺序访问的块，把仍驻留 slow 层者异步批量
晋升到 fast 层（与未命中块的 prefill 重叠，搬运开销被流水线遮蔽）——对应
C++ `TieredStorageManager::Prefetch` 的提前晋升语义（需求 2.6）。
"""

from __future__ import annotations

from dataclasses import dataclass

from bench.policies import make_policy


@dataclass(frozen=True)
class TieredCosts:
    """确定性开销模型（ms）。"""

    prefill_ms_per_block: float = 4.0
    promote_ms_per_block: float = 0.5  # slow→fast 层间搬运
    decode_ms_per_token: float = 0.05


@dataclass(frozen=True)
class RequestOutcome:
    """单请求回放结果；block_hits 与请求块序一一对应（fast 或 slow 命中为 True）。"""

    fast_hits: int
    slow_hits: int
    misses: int
    ttft_ms: float
    block_hits: tuple[bool, ...]


class TieredSimulator:
    """两级缓存模拟器：fast/slow 各由一个可插拔策略实例管理。"""

    def __init__(
        self,
        fast_blocks: int,
        slow_blocks: int,
        policy_name: str = "s3fifo",
        prefetch_depth: int = 8,
        costs: TieredCosts = TieredCosts(),
        slow_policy_name: str = "",
    ) -> None:
        self._fast = make_policy(policy_name, fast_blocks)
        self._slow = make_policy(slow_policy_name or policy_name, slow_blocks)
        self._fast.on_evict = self._slow.admit  # fast 淘汰 → 降级 slow
        self._successor: dict[int, int] = {}
        self._prefetch_depth = prefetch_depth
        self._costs = costs

    def process_request(self, hash_ids: tuple[int, ...]) -> RequestOutcome:
        """按序访问一个请求的全部块，返回命中分解与 TTFT。"""
        prefetched = self._prefetch_for_request(hash_ids)
        fast_hits = slow_hits = misses = 0
        block_hits: list[bool] = []
        prev: int | None = None
        for hash_id in hash_ids:
            if self._fast.contains(hash_id):
                self._fast.touch(hash_id)
                fast_hits += 1
                block_hits.append(True)
            elif self._slow.contains(hash_id):
                self._slow.remove(hash_id)
                self._fast.admit(hash_id)
                slow_hits += 1
                block_hits.append(True)
            else:
                self._fast.admit(hash_id)
                misses += 1
                block_hits.append(False)
            if prev is not None:
                self._successor[prev] = hash_id
            prev = hash_id
        ttft_ms = max(
            misses * self._costs.prefill_ms_per_block
            + slow_hits * self._costs.promote_ms_per_block,
            self._costs.prefill_ms_per_block,
        )
        del prefetched
        return RequestOutcome(
            fast_hits, slow_hits, misses, ttft_ms, tuple(block_hits)
        )

    def _prefetch_for_request(self, hash_ids: tuple[int, ...]) -> int:
        """请求到达时的异步批量晋升：以请求块序列中每个在存块为锚点，沿后继链
        把驻留 slow 层的预测块提前晋升到 fast 层；返回晋升块数。

        多锚点覆盖「前缀部分被冲刷、中段仍在存」的场景（对话续写常见），
        选型对比见 docs/benchmarks/REPORT.md。"""
        if self._prefetch_depth <= 0 or not hash_ids:
            return 0
        promoted = 0
        budget = len(hash_ids) + self._prefetch_depth
        for anchor in hash_ids:
            if not (
                self._fast.contains(anchor) or self._slow.contains(anchor)
            ):
                continue
            cursor = anchor
            for _ in range(budget):
                nxt = self._successor.get(cursor)
                if nxt is None:
                    break
                if self._slow.contains(nxt):
                    self._slow.remove(nxt)
                    self._fast.admit(nxt)
                    promoted += 1
                cursor = nxt
        return promoted
