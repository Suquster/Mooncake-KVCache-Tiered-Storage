"""vLLM v1 KV offloading 插件：S3-FIFO 淘汰策略（在线验证路径）。

把本项目 C++ 生产路径同款的 S3-FIFO 策略（小队列 10% 预算 + 主队列二次
机会 + 幽灵队列热准入，见 src/storage/tiered_storage_manager.cpp）以
vLLM `CachePolicy` 插件形式接入 OffloadingConnector 的 CPU 卸载层，
与内置 lru/arc 策略在真实推理负载下同条件对比。

不修改上游源码：经 `OffloadingSpecFactory` 的 `spec_module_path` 动态
加载（kv_connector_extra_config 指定本模块与 `S3FIFOOffloadingSpec`）。

用法（vllm serve）：
    --kv-transfer-config '{"kv_connector": "OffloadingConnector",
        "kv_role": "kv_both", "kv_connector_extra_config": {
        "spec_name": "S3FIFOOffloadingSpec",
        "spec_module_path": "vllm_adapter.offloading_spec",
        "cpu_bytes_to_use": 8000000000}}'
"""

from __future__ import annotations

from collections import OrderedDict
from collections.abc import Iterable

from typing_extensions import override

from vllm.v1.kv_offload.base import OffloadKey, ReqContext
from vllm.v1.kv_offload.cpu import manager as cpu_manager
from vllm.v1.kv_offload.cpu.policies.base import BlockStatus, CachePolicy
from vllm.v1.kv_offload.cpu.spec import CPUOffloadingSpec
from vllm.v1.kv_offload.tiering.spec import TieringOffloadingSpec

_FREQ_CAP = 3  # 频次上限（与 C++ s3_freq 上限一致）


class S3FIFOCachePolicy(CachePolicy):
    """S3-FIFO（SOSP'23）：一次性块优先淘汰，复用块经二次机会保留。

    - 小队列（容量 10% 预算）：新块先入小队列；淘汰时复用过（freq>0）
      的块晋升主队列，一次性块受害并记入幽灵队列。
    - 主队列：二次机会，freq>0 降频重入队尾，freq==0 受害。
    - 幽灵队列（2× 容量）：被逐一次性块的键；重新写入时命中幽灵则
      直接热准入主队列。
    """

    def __init__(self, cache_capacity: int):
        self.small_budget = max(1, cache_capacity // 10)
        self.ghost_capacity = max(1, 2 * cache_capacity)
        self.blocks: dict[OffloadKey, BlockStatus] = {}
        self.small: OrderedDict[OffloadKey, None] = OrderedDict()
        self.main: OrderedDict[OffloadKey, None] = OrderedDict()
        self.freq: dict[OffloadKey, int] = {}
        self.evictable: set[OffloadKey] = set()
        self.ghost: OrderedDict[OffloadKey, None] = OrderedDict()

    @override
    def get(self, key: OffloadKey) -> BlockStatus | None:
        return self.blocks.get(key)

    @override
    def insert(self, key: OffloadKey, block: BlockStatus) -> None:
        self.blocks[key] = block
        self.freq[key] = 0
        if self.ghost.pop(key, None) is not None:
            self.main[key] = None  # 幽灵命中 → 热准入主队列。
        else:
            self.small[key] = None
        if block.ref_cnt == 0:
            self.evictable.add(key)

    @override
    def remove(self, key: OffloadKey) -> None:
        del self.blocks[key]
        self.freq.pop(key, None)
        self.small.pop(key, None)
        self.main.pop(key, None)
        self.evictable.discard(key)

    @override
    def touch(self, keys: Iterable[OffloadKey], req_context: ReqContext) -> None:
        for key in keys:
            if key in self.freq:
                self.freq[key] = min(self.freq[key] + 1, _FREQ_CAP)

    @override
    def clear(self) -> None:
        self.blocks.clear()
        self.small.clear()
        self.main.clear()
        self.freq.clear()
        self.evictable.clear()
        self.ghost.clear()

    @override
    def evict(
        self, n: int, protected: set[OffloadKey]
    ) -> list[tuple[OffloadKey, BlockStatus]] | None:
        if n == 0:
            return []
        # (key, 是否来自小队列)；受保护/不可淘汰块暂移出，结束后按原序放回。
        victims: list[tuple[OffloadKey, bool]] = []
        skipped_small: list[OffloadKey] = []
        skipped_main: list[OffloadKey] = []
        while len(victims) < n:
            from_small = (
                len(self.small) > self.small_budget and self.small
            ) or not self.main
            if from_small:
                if not self.small:
                    break
                key, _ = self.small.popitem(last=False)
                if self.freq.get(key, 0) > 0:
                    self.freq[key] = 0
                    self.main[key] = None  # 复用过 → 晋升主队列。
                    continue
                if key in protected or key not in self.evictable:
                    skipped_small.append(key)
                    continue
                victims.append((key, True))
            else:
                key, _ = self.main.popitem(last=False)
                if self.freq.get(key, 0) > 0:
                    self.freq[key] -= 1  # 二次机会：降频重入队尾。
                    self.main[key] = None
                    continue
                if key in protected or key not in self.evictable:
                    skipped_main.append(key)
                    continue
                victims.append((key, False))

        succeeded = len(victims) == n
        restore_small = skipped_small + (
            [] if succeeded else [k for k, s in victims if s]
        )
        restore_main = skipped_main + (
            [] if succeeded else [k for k, s in victims if not s]
        )
        if restore_small:
            self.small = OrderedDict(
                [(k, None) for k in restore_small] + list(self.small.items())
            )
        if restore_main:
            self.main = OrderedDict(
                [(k, None) for k in restore_main] + list(self.main.items())
            )
        if not succeeded:
            return None
        result: list[tuple[OffloadKey, BlockStatus]] = []
        for key, from_small in victims:
            result.append((key, self.blocks.pop(key)))
            self.freq.pop(key, None)
            self.evictable.discard(key)
            if from_small:
                self._ghost_insert(key)
        return result

    @override
    def mark_evictable(self, key: OffloadKey) -> None:
        self.evictable.add(key)

    @override
    def mark_non_evictable(self, key: OffloadKey) -> None:
        self.evictable.discard(key)

    def _ghost_insert(self, key: OffloadKey) -> None:
        self.ghost[key] = None
        self.ghost.move_to_end(key)
        while len(self.ghost) > self.ghost_capacity:
            self.ghost.popitem(last=False)


class S3FIFOOffloadingSpec(CPUOffloadingSpec):
    """CPU 卸载 spec：把淘汰策略切换为本项目的 S3-FIFO。"""

    def __init__(self, config) -> None:
        super().__init__(config)
        cpu_manager._CACHE_POLICIES.setdefault("s3fifo", S3FIFOCachePolicy)
        self.eviction_policy = "s3fifo"


class S3FIFOTieringOffloadingSpec(TieringOffloadingSpec):
    """三层卸载 spec（GPU→CPU→NVMe/FS 等次级层）+ S3-FIFO 主层策略。

    次级层经 kv_connector_extra_config 的 `secondary_tiers` 配置（如
    `[{"type": "fs", "root_dir": "/data/kvcache"}]`），CPU 主层淘汰
    策略切换为本项目 S3-FIFO——被逐块降级到次级层而非丢弃，对应
    本项目 C++ 三层降级链（HBM→DRAM→NVMe）的在线形态。
    """

    def __init__(self, config) -> None:
        super().__init__(config)
        cpu_manager._CACHE_POLICIES.setdefault("s3fifo", S3FIFOCachePolicy)
        self.eviction_policy = "s3fifo"
