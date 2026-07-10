// 调度器实现（设计依据见头文件与 design.md「Scheduler」）。
#include "scheduler/scheduler.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace project::scheduler {

// ---------------------------------------------------------------------------
// CrossNodeIndex（需求 4.2 / 7.2）
// ---------------------------------------------------------------------------
void CrossNodeIndex::Register(const BlockKey& key, const NodeId& node) {
  Shard& shard = shards_[ShardOf(key)];
  std::lock_guard<std::mutex> lock(shard.mu);
  shard.holders[key].insert(node);
}

void CrossNodeIndex::Unregister(const BlockKey& key, const NodeId& node) {
  Shard& shard = shards_[ShardOf(key)];
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.holders.find(key);
  if (it == shard.holders.end()) {
    return;
  }
  it->second.erase(node);
  if (it->second.empty()) {
    shard.holders.erase(it);
  }
}

std::vector<NodeId> CrossNodeIndex::Lookup(const BlockKey& key) const {
  const Shard& shard = shards_[ShardOf(key)];
  std::lock_guard<std::mutex> lock(shard.mu);
  auto it = shard.holders.find(key);
  if (it == shard.holders.end()) {
    return {};
  }
  return {it->second.begin(), it->second.end()};
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
namespace {

// 节点是否过载（达到/超过其高水位阈值）。
bool Overloaded(const NodeState& node) {
  return node.occupancy_ratio >= node.high_water_ratio;
}

// 在候选中选占用率最低者（占用率并列时取 NodeId 字典序小者，保证决策确定）。
const NodeState* PickLeastLoaded(const std::vector<const NodeState*>& nodes) {
  const NodeState* best = nullptr;
  for (const NodeState* node : nodes) {
    if (best == nullptr || node->occupancy_ratio < best->occupancy_ratio ||
        (node->occupancy_ratio == best->occupancy_ratio &&
         node->id < best->id)) {
      best = node;
    }
  }
  return best;
}

}  // namespace

Scheduler::Scheduler(SchedulerConfig config, CrossNodeIndex& index)
    : config_(config), index_(index) {}

void Scheduler::UpdateClusterState(ClusterState state) {
  std::lock_guard<std::mutex> lock(mu_);
  cluster_ = std::move(state);
}

RouteDecision Scheduler::Route(const Request& req) {
  ClusterState snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    snapshot = cluster_;
  }
  if (config_.prefix_reuse_enabled && !req.prefix_keys.empty()) {
    // 统计各可达节点持有的前缀块数，选覆盖最多者（需求 4.1）。
    std::map<NodeId, std::size_t> coverage;
    for (const BlockKey& key : req.prefix_keys) {
      for (const NodeId& node : index_.Lookup(key)) {
        coverage[node] += 1;
      }
    }
    const NodeId* best = nullptr;
    std::size_t best_count = 0;
    for (const auto& [node_key, count] : coverage) {
      const NodeId& node = node_key;
      const auto state_it =
          std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                       [&node](const NodeState& s) { return s.id == node; });
      const bool reachable =
          state_it == snapshot.nodes.end() || state_it->reachable;
      if (reachable && count > best_count) {
        best = &node;
        best_count = count;
      }
    }
    if (best != nullptr) {
      return RouteDecision{*best, /*reuse_prefix=*/true, /*recompute=*/false};
    }
  }
  // 复用关闭或无前缀命中：负载均衡放置（不查询前缀索引，需求 4.6）。
  return RouteDecision{ChoosePlacementNode(snapshot), false, false};
}

Allocation Scheduler::AllocateFairShare(
    const std::vector<TenantDemand>& demands,
    std::uint64_t total_capacity_bytes) const {
  // 加权 max-min 公平（水填充）：迭代按权重比例分配，授予不超过需求；
  // 已满足者出局，其剩余容量再分配给尚有需求者（需求 4.3）。
  Allocation allocation;
  struct Item {
    const TenantDemand* demand;
    std::uint64_t granted = 0;
  };
  std::vector<Item> items;
  items.reserve(demands.size());
  for (const TenantDemand& demand : demands) {
    allocation.grants[demand.tenant] = 0;
    if (demand.demand_bytes > 0 && demand.weight > 0) {
      items.push_back(Item{&demand, 0});
    }
  }
  std::uint64_t remaining = total_capacity_bytes;
  while (remaining > 0) {
    double active_weight = 0;
    for (const Item& item : items) {
      if (item.granted < item.demand->demand_bytes) {
        active_weight += item.demand->weight;
      }
    }
    if (active_weight <= 0) {
      break;  // 所有需求已满足。
    }
    std::uint64_t distributed = 0;
    for (Item& item : items) {
      const std::uint64_t unmet = item.demand->demand_bytes - item.granted;
      if (unmet == 0) {
        continue;
      }
      const auto share = static_cast<std::uint64_t>(
          static_cast<double>(remaining) * item.demand->weight /
          active_weight);
      const std::uint64_t give = std::min<std::uint64_t>(unmet, share);
      item.granted += give;
      distributed += give;
    }
    if (distributed == 0) {
      // 剩余量过小无法按比例细分：轮转给尚有需求者，逐字节收尾。
      for (Item& item : items) {
        if (remaining == 0) {
          break;
        }
        if (item.granted < item.demand->demand_bytes) {
          item.granted += 1;
          distributed += 1;
          remaining -= 1;
        }
      }
      if (distributed == 0) {
        break;
      }
      continue;
    }
    remaining -= distributed;
  }
  for (const Item& item : items) {
    allocation.grants[item.demand->tenant] = item.granted;
  }
  return allocation;
}

NodeId Scheduler::ChoosePlacementNode(const ClusterState& state) const {
  std::vector<const NodeState*> reachable;
  std::vector<const NodeState*> below_water;
  for (const NodeState& node : state.nodes) {
    if (!node.reachable) {
      continue;
    }
    reachable.push_back(&node);
    if (!Overloaded(node)) {
      below_water.push_back(&node);
    }
  }
  // 需求 4.4：存在低于高水位的节点时，放置只在其中选取。
  const NodeState* chosen =
      PickLeastLoaded(below_water.empty() ? reachable : below_water);
  return chosen == nullptr ? NodeId{} : chosen->id;
}

RouteDecision Scheduler::ResolveOnFailure(const BlockKey& key,
                                          const ClusterState& state) const {
  std::vector<const NodeState*> reachable_holders;
  for (const NodeId& holder : index_.Lookup(key)) {
    const auto it =
        std::find_if(state.nodes.begin(), state.nodes.end(),
                     [&holder](const NodeState& s) { return s.id == holder; });
    if (it != state.nodes.end() && it->reachable) {
      reachable_holders.push_back(&*it);
    }
  }
  if (reachable_holders.empty()) {
    // 无可达持有者：示意重算（需求 4.5）。
    return RouteDecision{NodeId{}, false, /*recompute=*/true};
  }
  const NodeState* chosen = PickLeastLoaded(reachable_holders);
  return RouteDecision{chosen->id, /*reuse_prefix=*/true, false};
}

}  // namespace project::scheduler
