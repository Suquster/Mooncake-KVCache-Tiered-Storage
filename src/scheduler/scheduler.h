// =============================================================================
// src/scheduler/scheduler.h —— 控制面：跨节点索引、路由、公平配额、故障转移
// =============================================================================
// 设计依据：design.md「Scheduler」。控制面只做路由/索引/公平决策，
// 从不搬运块负载（数据面职责，见 datapath/）。
//
//   * CrossNodeIndex：块键 → 当前持有节点集合（需求 4.2），分片互斥量守护，
//     每次操作原子过渡（需求 7.2）。
//   * Route：前缀感知路由，复用已有前缀块的节点；配置关闭时不查询前缀索引
//     （需求 4.1 / 4.6）。
//   * AllocateFairShare：加权 max-min 公平（有剩余需求者不被饿死，富余容量
//     再分配，需求 4.3）。
//   * ChoosePlacementNode：有节点达到/超过高水位时，新放置避开之（需求 4.4）。
//   * ResolveOnFailure：首选持有者失联时改选可达持有者，否则示意重算
//     （需求 4.5）。
// =============================================================================
#ifndef PROJECT_SCHEDULER_SCHEDULER_H_
#define PROJECT_SCHEDULER_SCHEDULER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "project/status.h"
#include "project/types.h"

namespace project::scheduler {

// 租户标识（多租户公平配额的记账主体，需求 4.3）。
struct TenantId {
  std::string value;

  bool operator==(const TenantId& other) const = default;
  auto operator<=>(const TenantId& other) const = default;
};

// 节点运行状态：可达性（故障转移输入）与占用率（高水位输入）。
struct NodeState {
  NodeId id;
  bool reachable = true;
  double occupancy_ratio = 0.0;   // [0,1]，>= high_water 视为过载
  double high_water_ratio = 0.9;  // 该节点的高水位阈值
};

// 集群视图：调度决策的唯一输入快照。
struct ClusterState {
  std::vector<NodeState> nodes;
};

// 路由请求：前缀块键序列 + 租户。
struct Request {
  std::vector<BlockKey> prefix_keys;
  TenantId tenant;
};

// 路由决策。
struct RouteDecision {
  NodeId node;
  bool reuse_prefix = false;  // 命中前缀复用（需求 4.1）
  bool recompute = false;     // 故障转移到重算（需求 4.5）
};

// 单租户需求（加权 max-min 公平的输入）。
struct TenantDemand {
  TenantId tenant;
  std::uint64_t demand_bytes = 0;
  double weight = 1.0;  // 权重（>0）；默认等权
};

// 配额结果：租户 → 授予字节数。
struct Allocation {
  std::map<TenantId, std::uint64_t> grants;
};

// 跨节点索引：块键 → 持有节点集合（需求 4.2）。
// 并发：按键哈希分片，每分片一把互斥量；单键操作在分片内原子过渡（需求 7.2）。
class CrossNodeIndex {
 public:
  void Register(const BlockKey& key, const NodeId& node);
  void Unregister(const BlockKey& key, const NodeId& node);
  std::vector<NodeId> Lookup(const BlockKey& key) const;

 private:
  static constexpr std::size_t kShards = 16;
  std::size_t ShardOf(const BlockKey& key) const {
    return std::hash<BlockKey>{}(key) % kShards;
  }

  struct Shard {
    mutable std::mutex mu;
    std::unordered_map<BlockKey, std::set<NodeId>> holders;
  };
  std::array<Shard, kShards> shards_;
};

// 调度器配置。
struct SchedulerConfig {
  bool prefix_reuse_enabled = true;  // 需求 4.6：可配置关闭前缀复用
};

class Scheduler {
 public:
  Scheduler(SchedulerConfig config, CrossNodeIndex& index);

  // 更新集群视图（可达性/占用率），决策基于最近一次快照。
  void UpdateClusterState(ClusterState state);

  // 前缀感知路由（需求 4.1 / 4.6）：复用开启时选取持有最多前缀块的可达节点；
  // 关闭时不查询前缀索引，直接走负载均衡放置。
  RouteDecision Route(const Request& req);

  // 加权 max-min 公平配额（需求 4.3）：total_capacity_bytes 为可分配总量。
  Allocation AllocateFairShare(const std::vector<TenantDemand>& demands,
                               std::uint64_t total_capacity_bytes) const;

  // 负载均衡放置（需求 4.4）：存在低于高水位的可达节点时避开过载节点。
  NodeId ChoosePlacementNode(const ClusterState& state) const;

  // 故障转移（需求 4.5）：改选可达持有者；无可达持有者时 recompute=true。
  RouteDecision ResolveOnFailure(const BlockKey& key,
                                 const ClusterState& state) const;

 private:
  SchedulerConfig config_;
  CrossNodeIndex& index_;
  mutable std::mutex mu_;
  ClusterState cluster_;
};

}  // namespace project::scheduler

#endif  // PROJECT_SCHEDULER_SCHEDULER_H_
