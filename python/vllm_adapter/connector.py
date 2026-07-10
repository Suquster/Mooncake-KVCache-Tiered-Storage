"""vLLM_Adapter：经 Mooncake KV connector 契约接入项目分层存储（任务 11）。

设计依据 design.md「vLLM_Adapter (Python)」：把 vLLM KV-connector 的
store/load 回调翻译为 Scheduler 路由 + Data_Path 传输，并把项目的分层存储暴露为
connector 的后备 KV 池。**不修改上游源码**（需求 6.2），依赖锁定的 Mooncake 构建
（需求 6.3）。

本模块只依赖结构化协议（duck typing 的 Scheduler / TieredStore 抽象），
生产部署时经 pybind11 绑定（``_mooncake_kvcache``）注入 C++ 核心；
测试时注入内存态替身（见 tests/test_connector_integration.py）。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Protocol


@dataclass(frozen=True)
class BlockKey:
    """KVCache_Block 内容寻址键（与 C++ project::BlockKey 对齐）。"""

    hash_id: int
    layer: int = 0
    version: int = 1


@dataclass(frozen=True)
class Capabilities:
    """connector 上报的可用能力（需求 5.7 的能力探测输入）。"""

    rdma: bool = False
    gpu_direct: bool = False
    nvme: bool = False


@dataclass(frozen=True)
class ConnectorConfig:
    """connector 配置。"""

    prefix_reuse_enabled: bool = True
    capabilities: Capabilities = field(default_factory=Capabilities)


@dataclass(frozen=True)
class LoadResult:
    """load_kv 的结果：命中的块与未命中的键（未命中由调用方触发重算）。"""

    loaded: dict[BlockKey, bytes]
    missing: tuple[BlockKey, ...]


class TieredStore(Protocol):
    """项目分层存储的最小协议（由 pybind11 绑定或测试替身实现）。"""

    def put(self, key: BlockKey, payload: bytes) -> None: ...

    def get(self, key: BlockKey) -> bytes | None: ...

    def exists(self, key: BlockKey) -> bool: ...

    def prefetch(self, keys: list[BlockKey]) -> None: ...

    def record_sequence(self, keys: list[BlockKey]) -> None: ...

    def prefetch_chain(self, head: BlockKey, budget: int = 8) -> int: ...

    def prefetch_chain_multi(
        self, anchors: list[BlockKey], budget: int = 8
    ) -> int: ...


class Scheduler(Protocol):
    """控制面路由协议（由 pybind11 绑定或测试替身实现）。"""

    def register_block(self, key: BlockKey, node: str) -> None: ...

    def route(self, keys: list[BlockKey]) -> str: ...


class ProjectKVConnector:
    """实现 Mooncake KV connector 契约：store_kv / load_kv / supports。

    需求 6.1：vLLM 经 KV-connector 回调透明地读写项目的分层 KVCache 池。
    """

    def __init__(
        self,
        config: ConnectorConfig,
        scheduler: Scheduler,
        store: TieredStore,
        self_node: str = "127.0.0.1:0",
    ) -> None:
        self._config = config
        self._scheduler = scheduler
        self._store = store
        self._self_node = self_node

    def store_kv(
        self, request_id: str, block_keys: list[BlockKey], tensors: list[bytes]
    ) -> None:
        """把请求的 KV 块写入分层存储，并在跨节点索引登记持有关系。

        参数按位置一一对应：``block_keys[i]`` 的负载为 ``tensors[i]``。
        """
        if len(block_keys) != len(tensors):
            raise ValueError(
                f"块键与张量数量不一致：{len(block_keys)} != {len(tensors)}"
                f"（request_id={request_id}）"
            )
        for key, payload in zip(block_keys, tensors):
            self._store.put(key, payload)
            self._scheduler.register_block(key, self._self_node)
        if self._config.prefix_reuse_enabled:
            self._store.record_sequence(block_keys)

    def load_kv(self, request_id: str, block_keys: list[BlockKey]) -> LoadResult:
        """从分层存储装载请求的 KV 块；未命中键在结果中显式返回。

        装载前先批量预取：把驻留较慢层的目标块提前晋升到最快层（需求 2.6），
        层间搬运与未命中块的重算重叠。"""
        del request_id  # 仅用于日志/追踪；装载语义与请求无关。
        if self._config.prefix_reuse_enabled:
            self._store.prefetch(block_keys)
            if block_keys:
                # 前缀感知预取：以请求块序列中每个键为锚点，沿块后继图
                # 预测后续块并提前晋升（多锚点选型胜出，见基准报告）。
                self._store.prefetch_chain_multi(block_keys)
        loaded: dict[BlockKey, bytes] = {}
        missing: list[BlockKey] = []
        for key in block_keys:
            payload = self._store.get(key)
            if payload is None:
                missing.append(key)
            else:
                loaded[key] = payload
        return LoadResult(loaded=loaded, missing=tuple(missing))

    def supports(self) -> Capabilities:
        """上报 RDMA / GPUDirect / NVMe 可用性（需求 5.7 / 6.1）。"""
        return self._config.capabilities
