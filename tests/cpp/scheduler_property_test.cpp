// 调度器/并发属性测试（RapidCheck，最少 100 次迭代，Properties 9–13、17）。
#include <rapidcheck.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "scheduler/scheduler.h"

using project::BlockKey;
using project::NodeId;
using project::scheduler::Allocation;
using project::scheduler::ClusterState;
using project::scheduler::CrossNodeIndex;
using project::scheduler::NodeState;
using project::scheduler::Request;
using project::scheduler::Scheduler;
using project::scheduler::SchedulerConfig;
using project::scheduler::TenantDemand;
using project::scheduler::TenantId;

namespace {

NodeId Node(int i) { return NodeId{"10.0.0." + std::to_string(i) + ":8080"}; }

}  // namespace

int main() {
  bool ok = true;

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 9: "
      "prefix-aware routing reuses existing blocks",
      [] {
        CrossNodeIndex index;
        const auto holder = Node(*rc::gen::inRange(1, 5));
        const auto prefix_len = *rc::gen::inRange<std::size_t>(1, 8);
        std::vector<BlockKey> prefix;
        for (std::size_t i = 0; i < prefix_len; ++i) {
          prefix.push_back(BlockKey{static_cast<std::uint64_t>(i), 0});
          index.Register(prefix.back(), holder);
        }
        ClusterState cluster;
        for (int i = 1; i <= 5; ++i) {
          cluster.nodes.push_back(NodeState{Node(i), true, 0.1, 0.9});
        }
        // 复用开启：路由到持有前缀块的节点（需求 4.1）。
        Scheduler enabled(SchedulerConfig{true}, index);
        enabled.UpdateClusterState(cluster);
        const auto decision = enabled.Route(Request{prefix, TenantId{"t"}});
        RC_ASSERT(decision.reuse_prefix);
        RC_ASSERT(decision.node == holder);
        // 复用关闭：不查询前缀索引，决策不带 reuse 标志（需求 4.6）。
        Scheduler disabled(SchedulerConfig{false}, index);
        disabled.UpdateClusterState(cluster);
        const auto decision2 = disabled.Route(Request{prefix, TenantId{"t"}});
        RC_ASSERT(!decision2.reuse_prefix);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 10: "
      "cross-node index round-trip",
      [] {
        CrossNodeIndex index;
        // 参考模型：串行 map；随机 register/unregister 序列后 Lookup 必须一致。
        std::map<BlockKey, std::set<NodeId>> model;
        const auto ops = *rc::gen::container<std::vector<std::tuple<bool, int, int>>>(
            rc::gen::tuple(rc::gen::arbitrary<bool>(), rc::gen::inRange(0, 6),
                           rc::gen::inRange(1, 5)));
        for (const auto& [is_register, key_i, node_i] : ops) {
          const BlockKey key{static_cast<std::uint64_t>(key_i), 0};
          const NodeId node = Node(node_i);
          if (is_register) {
            index.Register(key, node);
            model[key].insert(node);
          } else {
            index.Unregister(key, node);
            model[key].erase(node);
          }
        }
        for (int key_i = 0; key_i < 6; ++key_i) {
          const BlockKey key{static_cast<std::uint64_t>(key_i), 0};
          auto holders = index.Lookup(key);
          std::set<NodeId> got(holders.begin(), holders.end());
          RC_ASSERT(got == model[key]);
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 11: "
      "fair-share allocation",
      [] {
        CrossNodeIndex index;
        Scheduler scheduler(SchedulerConfig{}, index);
        const auto tenant_count = *rc::gen::inRange<int>(1, 6);
        const auto capacity = *rc::gen::inRange<std::uint64_t>(0, 10000);
        std::vector<TenantDemand> demands;
        std::uint64_t total_demand = 0;
        for (int i = 0; i < tenant_count; ++i) {
          const auto demand = *rc::gen::inRange<std::uint64_t>(0, 4000);
          demands.push_back(
              TenantDemand{TenantId{"tenant-" + std::to_string(i)}, demand,
                           1.0});
          total_demand += demand;
        }
        const Allocation allocation =
            scheduler.AllocateFairShare(demands, capacity);
        std::uint64_t granted_total = 0;
        std::uint64_t min_unmet_grant = UINT64_MAX;
        std::uint64_t max_unmet_grant = 0;
        for (const TenantDemand& demand : demands) {
          const auto grant = allocation.grants.at(demand.tenant);
          // 授予不超过需求。
          RC_ASSERT(grant <= demand.demand_bytes);
          granted_total += grant;
          if (grant < demand.demand_bytes) {
            min_unmet_grant = std::min(min_unmet_grant, grant);
            max_unmet_grant = std::max(max_unmet_grant, grant);
          }
        }
        // 总授予不超过容量；需求足够时容量被吃满（富余再分配，无浪费）。
        RC_ASSERT(granted_total <= capacity);
        RC_ASSERT(granted_total == std::min(capacity, total_demand));
        // 无饿死：等权下任意两个「未满足」租户的授予差不超过 1 字节
        // （max-min 公平的判据）。
        if (max_unmet_grant != 0 && min_unmet_grant != UINT64_MAX) {
          RC_ASSERT(max_unmet_grant - min_unmet_grant <= 1);
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 12: "
      "load-balancing redirect",
      [] {
        CrossNodeIndex index;
        Scheduler scheduler(SchedulerConfig{}, index);
        ClusterState cluster;
        const auto node_count = *rc::gen::inRange<int>(2, 8);
        // 构造性保证：至少一个低水位节点与一个高水位节点并存。
        cluster.nodes.push_back(
            NodeState{Node(1), true,
                      static_cast<double>(*rc::gen::inRange<int>(0, 90)) / 100.0,
                      0.9});
        cluster.nodes.push_back(
            NodeState{Node(2), true,
                      static_cast<double>(*rc::gen::inRange<int>(90, 101)) /
                          100.0,
                      0.9});
        for (int i = 3; i <= node_count; ++i) {
          const double occupancy =
              static_cast<double>(*rc::gen::inRange<int>(0, 101)) / 100.0;
          cluster.nodes.push_back(NodeState{Node(i), true, occupancy, 0.9});
        }
        const NodeId chosen = scheduler.ChoosePlacementNode(cluster);
        const auto it = std::find_if(
            cluster.nodes.begin(), cluster.nodes.end(),
            [&chosen](const NodeState& s) { return s.id == chosen; });
        RC_ASSERT(it != cluster.nodes.end());
        // 需求 4.4：存在低水位节点时放置必须避开高水位节点。
        RC_ASSERT(it->occupancy_ratio < it->high_water_ratio);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 13: "
      "failover resolution",
      [] {
        CrossNodeIndex index;
        Scheduler scheduler(SchedulerConfig{}, index);
        const BlockKey key{*rc::gen::arbitrary<std::uint64_t>(), 0};
        const auto holder_count = *rc::gen::inRange<int>(1, 5);
        ClusterState cluster;
        bool any_reachable = false;
        for (int i = 1; i <= holder_count; ++i) {
          index.Register(key, Node(i));
          const bool reachable = *rc::gen::arbitrary<bool>();
          cluster.nodes.push_back(NodeState{Node(i), reachable, 0.5, 0.9});
          any_reachable |= reachable;
        }
        const auto decision = scheduler.ResolveOnFailure(key, cluster);
        if (any_reachable) {
          // 改选可达持有者（需求 4.5）。
          RC_ASSERT(!decision.recompute);
          const auto it = std::find_if(
              cluster.nodes.begin(), cluster.nodes.end(),
              [&decision](const NodeState& s) { return s.id == decision.node; });
          RC_ASSERT(it != cluster.nodes.end());
          RC_ASSERT(it->reachable);
        } else {
          // 无可达持有者：示意重算。
          RC_ASSERT(decision.recompute);
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 17: "
      "concurrency consistency of shared state",
      [] {
        CrossNodeIndex index;
        // 每线程独占一个节点身份，对共享键集并发 register/unregister；
        // 单键单节点的操作序全部由同一线程决定，故终态等价于某个串行执行。
        const auto thread_count = *rc::gen::inRange<int>(2, 6);
        const auto key_count = *rc::gen::inRange<int>(1, 8);
        // 每线程对每个键的最终动作：true=保留注册，false=注销。
        std::vector<std::vector<bool>> final_state;
        for (int t = 0; t < thread_count; ++t) {
          final_state.push_back(*rc::gen::container<std::vector<bool>>(
              static_cast<std::size_t>(key_count), rc::gen::arbitrary<bool>()));
        }
        std::vector<std::thread> threads;
        for (int t = 0; t < thread_count; ++t) {
          threads.emplace_back([&, t] {
            const NodeId self = Node(t + 1);
            for (int k = 0; k < key_count; ++k) {
              const BlockKey key{static_cast<std::uint64_t>(k), 0};
              // 混合操作序列，终态由 final_state 决定。
              index.Register(key, self);
              index.Unregister(key, self);
              if (final_state[t][k]) {
                index.Register(key, self);
              }
            }
          });
        }
        for (auto& thread : threads) {
          thread.join();
        }
        for (int k = 0; k < key_count; ++k) {
          const BlockKey key{static_cast<std::uint64_t>(k), 0};
          std::set<NodeId> expected;
          for (int t = 0; t < thread_count; ++t) {
            if (final_state[t][k]) {
              expected.insert(Node(t + 1));
            }
          }
          const auto holders = index.Lookup(key);
          RC_ASSERT(std::set<NodeId>(holders.begin(), holders.end()) ==
                    expected);
        }
      });

  return ok ? 0 : 1;
}
