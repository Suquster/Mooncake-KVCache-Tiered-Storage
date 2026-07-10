# pybind11 绑定端到端测试：Python connector 直接驱动 C++ 核心（生产接线）。
# 绑定模块由 CI 构建后经 PYTHONPATH=build/src 注入；本地未构建时跳过。
import pytest

_mooncake = pytest.importorskip("_mooncake_kvcache")

from vllm_adapter.connector import Capabilities, ConnectorConfig, ProjectKVConnector


def test_store_roundtrip_and_locate() -> None:
    store = _mooncake.TieredStore()
    key = _mooncake.BlockKey(42)
    store.put(key, b"hello-kv")
    assert store.get(key) == b"hello-kv"
    assert store.exists(key)
    assert store.get(_mooncake.BlockKey(999)) is None


def test_scheduler_prefix_route() -> None:
    scheduler = _mooncake.Scheduler()
    key = _mooncake.BlockKey(7)
    scheduler.update_cluster([("10.0.0.1:8080", True, 0.3), ("10.0.0.2:8080", True, 0.3)])
    scheduler.register_block(key, "10.0.0.1:8080")
    assert scheduler.route([key]) == "10.0.0.1:8080"


def test_connector_end_to_end_with_cpp_core() -> None:
    store = _mooncake.TieredStore()
    scheduler = _mooncake.Scheduler()
    scheduler.update_cluster([("node-a", True, 0.1)])
    connector = ProjectKVConnector(
        ConnectorConfig(capabilities=Capabilities(rdma=True)),
        scheduler,
        store,
        self_node="node-a",
    )
    keys = [_mooncake.BlockKey(1), _mooncake.BlockKey(2)]
    connector.store_kv("req-1", keys, [b"k1v1", b"k2v2"])
    result = connector.load_kv("req-1", keys + [_mooncake.BlockKey(3)])
    assert set(result.loaded.values()) == {b"k1v1", b"k2v2"}
    assert len(result.missing) == 1


def test_prefetch_promotes_without_data_loss() -> None:
    """prefetch 提前晋升不丢内容：预取后仍可原样读回。"""
    store = _mooncake.TieredStore()
    keys = [_mooncake.BlockKey(11), _mooncake.BlockKey(12)]
    store.put(keys[0], b"payload-a")
    store.put(keys[1], b"payload-b")
    store.prefetch(keys + [_mooncake.BlockKey(999)])  # 不在存的键被静默跳过
    assert store.get(keys[0]) == b"payload-a"
    assert store.get(keys[1]) == b"payload-b"


def test_prefetch_chain_promotes_recorded_successors() -> None:
    """record_sequence 记录后继图；prefetch_chain 从锚点预测并晋升预测链。"""
    store = _mooncake.TieredStore()
    keys = [_mooncake.BlockKey(21), _mooncake.BlockKey(22), _mooncake.BlockKey(23)]
    for i, key in enumerate(keys):
        store.put(key, f"payload-{i}".encode())
    store.record_sequence(keys)
    assert store.prefetch_chain(keys[0], budget=8) == 2
    assert store.prefetch_chain(_mooncake.BlockKey(999), budget=8) == 0
    for i, key in enumerate(keys):
        assert store.get(key) == f"payload-{i}".encode()


def test_prefetch_chain_multi_promotes_deduplicated_union() -> None:
    """prefetch_chain_multi 以多锚点预测并晋升去重后的后继链并集。"""
    store = _mooncake.TieredStore()
    keys = [_mooncake.BlockKey(31), _mooncake.BlockKey(32), _mooncake.BlockKey(33)]
    for i, key in enumerate(keys):
        store.put(key, f"payload-{i}".encode())
    store.record_sequence(keys)
    # 锚点 31 预测 {32,33}，锚点 32 预测 {33}（去重后并集大小 2）。
    assert store.prefetch_chain_multi(keys[:2], budget=8) == 2
    assert store.prefetch_chain_multi([_mooncake.BlockKey(999)], budget=8) == 0
    for i, key in enumerate(keys):
        assert store.get(key) == f"payload-{i}".encode()


def test_version_provenance() -> None:
    assert _mooncake.project_version()
    assert _mooncake.mooncake_required_version()
    assert len(_mooncake.mooncake_required_commit()) >= 12
