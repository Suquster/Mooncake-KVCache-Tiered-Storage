// =============================================================================
// pybind11 绑定模块 _mooncake_kvcache —— Python↔C++ 语言边界
// =============================================================================
// 设计（见 design.md「Language boundary」）：Python 编排层（vLLM 适配、基准框架）
// 通过本绑定调用 C++ 核心。暴露面刻意最小化：Python 侧
// （vllm_adapter.connector）只依赖 TieredStore / Scheduler 两个 Protocol，
// 本模块提供其生产实现：
//   * TieredStore —— 包装 TieredStorageManager（bytes 负载 ↔ KVCacheBlock）
//   * Scheduler   —— 包装 CrossNodeIndex + Scheduler（路由/登记）
// 错误经 Result/Status 翻译为 Python 边界上的 None / RuntimeError。
// =============================================================================
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "project/core.h"
#include "scheduler/scheduler.h"
#include "storage/prefix_prefetcher.h"
#include "storage/tiered_storage_manager.h"

namespace py = pybind11;

namespace {

using project::BlockKey;
using project::DType;
using project::KVCacheBlock;
using project::NodeId;
using project::storage::PrefixPrefetcher;
using project::storage::Tier;
using project::storage::TierConfig;
using project::storage::TieredStorageManager;

// bytes 负载 → KVCacheBlock（K/V 平分负载）。
KVCacheBlock BlockFromPayload(const BlockKey& key, const py::bytes& payload) {
  KVCacheBlock block;
  block.key = key;
  block.dtype = DType::kFloat16;
  std::string data(payload);
  const std::size_t half = data.size() / 2;
  block.k_payload.resize(half);
  block.v_payload.resize(data.size() - half);
  std::memcpy(block.k_payload.data(), data.data(), half);
  std::memcpy(block.v_payload.data(), data.data() + half, data.size() - half);
  return block;
}

py::bytes PayloadFromBlock(const KVCacheBlock& block) {
  std::string data(block.k_payload.size() + block.v_payload.size(), '\0');
  std::memcpy(data.data(), block.k_payload.data(), block.k_payload.size());
  std::memcpy(data.data() + block.k_payload.size(), block.v_payload.data(),
              block.v_payload.size());
  return py::bytes(data);
}

// TieredStore Protocol 的生产实现（vllm_adapter.connector.TieredStore）。
class PyTieredStore {
 public:
  PyTieredStore(std::optional<std::uint64_t> hbm,
                std::optional<std::uint64_t> dram,
                std::optional<std::uint64_t> nvme, double high_water_ratio)
      : manager_([&] {
          TierConfig config;
          config.hbm_capacity_bytes = hbm;
          config.dram_capacity_bytes = dram;
          config.nvme_capacity_bytes = nvme;
          config.high_water_ratio = high_water_ratio;
          return config;
        }()) {}

  void put(const BlockKey& key, const py::bytes& payload) {
    const auto status =
        manager_.Write(BlockFromPayload(key, payload), Tier::kHBM);
    if (!status.ok()) {
      throw std::runtime_error(status.message());
    }
  }

  std::optional<py::bytes> get(const BlockKey& key) {
    auto result = manager_.Read(key);
    if (!result.ok()) {
      return std::nullopt;
    }
    return PayloadFromBlock(result.value());
  }

  bool exists(const BlockKey& key) { return manager_.Locate(key).hit; }

  void prefetch(const std::vector<BlockKey>& keys) {
    const auto status = manager_.Prefetch(keys);
    if (!status.ok()) {
      throw std::runtime_error(status.message());
    }
  }

  // 记录请求块序列的相邻关系（块后继图，前缀感知预取的预测依据）。
  void record_sequence(const std::vector<BlockKey>& keys) {
    prefetcher_.RecordSequence(keys);
  }

  // 从 head 沿后继链预测并批量晋升至最快层；返回预测链长度。
  std::size_t prefetch_chain(const BlockKey& head, std::size_t budget) {
    const auto chain = prefetcher_.PredictChain(head, budget);
    if (!chain.empty()) {
      prefetch(chain);
    }
    return chain.size();
  }

  // 多锚点链式预取：以 anchors 中每个键为锚点预测后继链，合并去重后
  // 批量晋升；返回预测块数（多锚点选型胜出，见基准报告）。
  std::size_t prefetch_chain_multi(const std::vector<BlockKey>& anchors,
                                   std::size_t budget) {
    const auto chain = prefetcher_.PredictChainMulti(anchors, budget);
    if (!chain.empty()) {
      prefetch(chain);
    }
    return chain.size();
  }

 private:
  TieredStorageManager manager_;
  PrefixPrefetcher prefetcher_;
};

// Scheduler Protocol 的生产实现（vllm_adapter.connector.Scheduler）。
class PyScheduler {
 public:
  explicit PyScheduler(bool prefix_reuse_enabled)
      : scheduler_(project::scheduler::SchedulerConfig{prefix_reuse_enabled},
                   index_) {}

  void register_block(const BlockKey& key, const std::string& node) {
    index_.Register(key, NodeId{node});
  }

  std::string route(const std::vector<BlockKey>& keys) {
    const auto decision = scheduler_.Route(
        project::scheduler::Request{keys, project::scheduler::TenantId{}});
    return decision.node.value;
  }

  void update_cluster(
      const std::vector<std::tuple<std::string, bool, double>>& nodes) {
    project::scheduler::ClusterState state;
    for (const auto& [id, reachable, occupancy] : nodes) {
      state.nodes.push_back(project::scheduler::NodeState{
          NodeId{id}, reachable, occupancy, 0.9});
    }
    scheduler_.UpdateClusterState(std::move(state));
  }

 private:
  project::scheduler::CrossNodeIndex index_;
  project::scheduler::Scheduler scheduler_;
};

}  // namespace

PYBIND11_MODULE(_mooncake_kvcache, m) {
  m.doc() = "Mooncake KVCache 优化项目的 C++ 核心绑定（版本与依赖溯源接口）";

  // 暴露编译期固化的版本信息，供 Python 侧做依赖一致性校验与报告溯源。
  m.def("project_version", &project::ProjectVersion,
        "返回本项目语义化版本号。");
  m.def("mooncake_required_version", &project::MooncakeRequiredVersion,
        "返回 mooncake.lock 锁定的上游 Mooncake 版本号。");
  m.def("mooncake_required_commit", &project::MooncakeRequiredCommit,
        "返回 mooncake.lock 锁定的上游 Mooncake commit 哈希。");

  py::class_<BlockKey>(m, "BlockKey")
      .def(py::init([](std::uint64_t hash_id, std::uint32_t layer,
                       std::uint16_t version) {
             return BlockKey{hash_id, layer, version};
           }),
           py::arg("hash_id"), py::arg("layer") = 0, py::arg("version") = 1)
      .def_readonly("hash_id", &BlockKey::hash_id)
      .def_readonly("layer", &BlockKey::layer)
      .def_readonly("version", &BlockKey::version)
      .def("__eq__",
           [](const BlockKey& a, const BlockKey& b) { return a == b; })
      .def("__hash__",
           [](const BlockKey& key) { return std::hash<BlockKey>{}(key); });

  py::class_<PyTieredStore>(m, "TieredStore")
      .def(py::init<std::optional<std::uint64_t>, std::optional<std::uint64_t>,
                    std::optional<std::uint64_t>, double>(),
           py::arg("hbm_bytes") = std::optional<std::uint64_t>(1ULL << 30),
           py::arg("dram_bytes") = std::optional<std::uint64_t>(4ULL << 30),
           py::arg("nvme_bytes") = std::nullopt,
           py::arg("high_water_ratio") = 0.9)
      .def("put", &PyTieredStore::put)
      .def("get", &PyTieredStore::get)
      .def("exists", &PyTieredStore::exists)
      .def("prefetch", &PyTieredStore::prefetch)
      .def("record_sequence", &PyTieredStore::record_sequence)
      .def("prefetch_chain", &PyTieredStore::prefetch_chain, py::arg("head"),
           py::arg("budget") = 8)
      .def("prefetch_chain_multi", &PyTieredStore::prefetch_chain_multi,
           py::arg("anchors"), py::arg("budget") = 8);

  py::class_<PyScheduler>(m, "Scheduler")
      .def(py::init<bool>(), py::arg("prefix_reuse_enabled") = true)
      .def("register_block", &PyScheduler::register_block)
      .def("route", &PyScheduler::route)
      .def("update_cluster", &PyScheduler::update_cluster);
}
