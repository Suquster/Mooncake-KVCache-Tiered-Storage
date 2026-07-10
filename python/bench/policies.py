"""可插拔缓存策略：LRU / SIEVE / S3-FIFO / 前缀感知 LRU（策略选型基座）。

统一接口：``access(hash_id, block_index) -> hit``，一次调用完成
查找 + 准入 + 淘汰。所有策略确定性（无随机数），保证回放可复现（需求 5.6）。

前缀感知（prefix-aware）动机：KVCache 块只有作为「连续前缀」的一部分才可复用
（vLLM 前缀缓存语义）——序列尾部块的边际价值低于头部块。PrefixAwareLRU 在淘汰
时优先牺牲「块深度大」（block_index 大）的条目，保护热门前缀头部。
"""

from __future__ import annotations

from collections import OrderedDict

_MISSING = object()


class CachePolicy:
    """策略基类：容量以块数计。

    可选分层接口（供 bench.tiered 两级模拟使用）：``contains`` / ``touch`` /
    ``admit`` / ``remove``，以及淘汰回调 ``on_evict``（仅在条目被彻底逐出时
    触发，降级/预取的接受方据此接管块）。
    """

    name = "base"

    def __init__(self, capacity: int) -> None:
        if capacity <= 0:
            raise ValueError("capacity 必须为正")
        self.capacity = capacity
        self.on_evict = None  # 可选回调：Callable[[int], None]

    def access(self, hash_id: int, block_index: int = 0) -> bool:
        raise NotImplementedError

    def contains(self, hash_id: int) -> bool:
        raise NotImplementedError

    def touch(self, hash_id: int) -> None:
        """命中时的元数据更新（不改变驻留集）。"""
        raise NotImplementedError

    def admit(self, hash_id: int) -> None:
        """直接准入一个键（预取/降级路径），必要时触发淘汰。"""
        raise NotImplementedError

    def remove(self, hash_id: int) -> bool:
        """移除一个键（晋升路径）；返回是否存在。"""
        raise NotImplementedError

    def _evicted(self, hash_id: int) -> None:
        if self.on_evict is not None:
            self.on_evict(hash_id)


class LRUPolicy(CachePolicy):
    """经典 LRU（基线参照）。"""

    name = "lru"

    def __init__(self, capacity: int) -> None:
        super().__init__(capacity)
        self._store: OrderedDict[int, None] = OrderedDict()

    def access(self, hash_id: int, block_index: int = 0) -> bool:
        del block_index
        hit = hash_id in self._store
        if hit:
            self._store.move_to_end(hash_id)
        else:
            self.admit(hash_id)
        return hit

    def contains(self, hash_id: int) -> bool:
        return hash_id in self._store

    def touch(self, hash_id: int) -> None:
        if hash_id in self._store:
            self._store.move_to_end(hash_id)

    def admit(self, hash_id: int) -> None:
        self._store[hash_id] = None
        self._store.move_to_end(hash_id)
        while len(self._store) > self.capacity:
            victim, _ = self._store.popitem(last=False)
            self._evicted(victim)

    def remove(self, hash_id: int) -> bool:
        return self._store.pop(hash_id, _MISSING) is not _MISSING


class SievePolicy(CachePolicy):
    """SIEVE（NSDI'24）：FIFO 序 + visited 位 + 淘汰指针，扫描抵抗且零移动开销。"""

    name = "sieve"

    def __init__(self, capacity: int) -> None:
        super().__init__(capacity)
        self._store: OrderedDict[int, bool] = OrderedDict()  # key → visited
        self._cursor: int | None = None  # 淘汰手指所指 key

    def access(self, hash_id: int, block_index: int = 0) -> bool:
        del block_index
        if hash_id in self._store:
            self._store[hash_id] = True
            return True
        self.admit(hash_id)
        return False

    def contains(self, hash_id: int) -> bool:
        return hash_id in self._store

    def touch(self, hash_id: int) -> None:
        if hash_id in self._store:
            self._store[hash_id] = True

    def admit(self, hash_id: int) -> None:
        if hash_id in self._store:
            return
        if len(self._store) >= self.capacity:
            self._evict()
        self._store[hash_id] = False

    def remove(self, hash_id: int) -> bool:
        return self._store.pop(hash_id, _MISSING) is not _MISSING

    def _evict(self) -> None:
        keys = list(self._store.keys())
        if not keys:
            return
        # 手指从上次位置继续；visited 的条目获得二次机会（清位后跳过）。
        start = keys.index(self._cursor) if self._cursor in self._store else 0
        n = len(keys)
        i = start
        while True:
            key = keys[i % n]
            if self._store.get(key, False):
                self._store[key] = False
                i += 1
            else:
                self._cursor = keys[(i + 1) % n] if n > 1 else None
                self._store.pop(key, None)
                self._evicted(key)
                return


class S3FifoPolicy(CachePolicy):
    """S3-FIFO（SOSP'23）：小队列(10%) + 主队列(90%) + 幽灵队列。

    一次性访问在小队列即被淘汰，不污染主队列；幽灵队列命中者直接进主队列。
    """

    name = "s3fifo"

    def __init__(self, capacity: int) -> None:
        super().__init__(capacity)
        self._small_cap = max(1, capacity // 10)
        self._main_cap = capacity - self._small_cap
        self._small: OrderedDict[int, int] = OrderedDict()  # key → freq
        self._main: OrderedDict[int, int] = OrderedDict()
        self._ghost: OrderedDict[int, None] = OrderedDict()
        # 幽灵队列容量 = 2×总容量：tiered 模式参数扫描胜出（四 trace 均 +1%，
        # 见 docs/benchmarks/REPORT.md）；幽灵队列仅存键，内存成本可忽。
        self._ghost_cap = capacity * 2

    def access(self, hash_id: int, block_index: int = 0) -> bool:
        del block_index
        if hash_id in self._small:
            self._small[hash_id] = min(self._small[hash_id] + 1, 3)
            return True
        if hash_id in self._main:
            self._main[hash_id] = min(self._main[hash_id] + 1, 3)
            return True
        # 未命中：幽灵命中 → 主队列；否则进小队列。
        self.admit(hash_id)
        return False

    def contains(self, hash_id: int) -> bool:
        return hash_id in self._small or hash_id in self._main

    def touch(self, hash_id: int) -> None:
        if hash_id in self._small:
            self._small[hash_id] = min(self._small[hash_id] + 1, 3)
        elif hash_id in self._main:
            self._main[hash_id] = min(self._main[hash_id] + 1, 3)

    def admit(self, hash_id: int) -> None:
        if self.contains(hash_id):
            return
        if hash_id in self._ghost:
            self._ghost.pop(hash_id)
            self._insert_main(hash_id)
        else:
            self._insert_small(hash_id)

    def remove(self, hash_id: int) -> bool:
        if self._small.pop(hash_id, _MISSING) is not _MISSING:
            return True
        return self._main.pop(hash_id, _MISSING) is not _MISSING

    def _insert_small(self, key: int) -> None:
        self._small[key] = 0
        while len(self._small) > self._small_cap:
            victim, freq = self._small.popitem(last=False)
            if freq > 0:
                self._insert_main(victim)  # 复用过 → 晋升主队列
            else:
                self._ghost[victim] = None  # 一次性访问 → 幽灵队列
                while len(self._ghost) > self._ghost_cap:
                    self._ghost.popitem(last=False)
                self._evicted(victim)

    def _insert_main(self, key: int) -> None:
        self._main[key] = 0
        while len(self._main) > self._main_cap:
            victim, freq = self._main.popitem(last=False)
            if freq > 0:
                self._main[victim] = freq - 1  # 二次机会：降频后重入队尾
            else:
                self._evicted(victim)  # freq == 0 → 淘汰（不进幽灵队列）


class PrefixAwareLRUPolicy(CachePolicy):
    """前缀感知 LRU：淘汰时在最旧的 K 个候选中优先牺牲块深度最大者。

    直觉：深度大的块只有其整个前缀都在缓存时才有价值，是「最贵重建、最少复用」
    的部分；先淘汰它们能为热门前缀头部腾出空间。
    """

    name = "prefix_lru"

    _CANDIDATE_WINDOW = 16

    def __init__(self, capacity: int) -> None:
        super().__init__(capacity)
        self._store: OrderedDict[int, int] = OrderedDict()  # key → block_index

    def access(self, hash_id: int, block_index: int = 0) -> bool:
        hit = hash_id in self._store
        if hit:
            self._store.move_to_end(hash_id)
            self._store[hash_id] = block_index
        else:
            self.admit(hash_id, block_index)
        return hit

    def contains(self, hash_id: int) -> bool:
        return hash_id in self._store

    def touch(self, hash_id: int) -> None:
        if hash_id in self._store:
            self._store.move_to_end(hash_id)

    def admit(self, hash_id: int, block_index: int = 0) -> None:
        self._store[hash_id] = block_index
        self._store.move_to_end(hash_id)
        while len(self._store) > self.capacity:
            self._evict()

    def remove(self, hash_id: int) -> bool:
        return self._store.pop(hash_id, _MISSING) is not _MISSING

    def _evict(self) -> None:
        # 在 LRU 端的候选窗口内选块深度最大者。
        candidates = []
        for key in self._store:
            candidates.append(key)
            if len(candidates) >= self._CANDIDATE_WINDOW:
                break
        victim = max(candidates, key=lambda k: self._store[k])
        self._store.pop(victim)
        self._evicted(victim)


class S3FifoPrefixPolicy(S3FifoPolicy):
    """S3-FIFO + 前缀感知准入：深度超过准入上限的块直接绕过缓存。

    深尾块（block_index 大）复用概率极低，直接不准入可把容量留给前缀头部；
    绕过的访问仍记为未命中，不影响正确性（读路径回退重算）。
    """

    name = "s3fifo_prefix"

    _ADMIT_DEPTH_FACTOR = 4  # 准入深度上限 = capacity // factor

    def __init__(self, capacity: int) -> None:
        super().__init__(capacity)
        self._admit_depth = max(8, capacity // self._ADMIT_DEPTH_FACTOR)

    def access(self, hash_id: int, block_index: int = 0) -> bool:
        if hash_id not in self._small and hash_id not in self._main:
            if block_index > self._admit_depth and hash_id not in self._ghost:
                return False  # 深尾块：绕过准入
        return super().access(hash_id, block_index)


POLICIES: dict[str, type[CachePolicy]] = {
    cls.name: cls
    for cls in (
        LRUPolicy,
        SievePolicy,
        S3FifoPolicy,
        PrefixAwareLRUPolicy,
        S3FifoPrefixPolicy,
    )
}


def make_policy(name: str, capacity: int) -> CachePolicy:
    """按名称实例化策略。"""
    try:
        return POLICIES[name](capacity)
    except KeyError as exc:
        raise ValueError(f"未知策略：{name}（可选：{sorted(POLICIES)}）") from exc
