// =============================================================================
// src/storage/tiered_storage_manager.h —— 分层存储管理器（HBM▸DRAM▸NVMe）
// =============================================================================
// 设计依据：design.md「Tiered_Storage_Manager」。管理 KVCache_Block 在各层间的
// 放置与流动，驱动 Prefetch（提前晋升）与 Eviction（分段 LRU + 频次加权淘汰）。
//
// 关键不变量：
//   * 单层放置（需求 2.1）：任一在存块恰好解析到一个权威层。
//   * 阈值放置（需求 2.2）：目标层占用低于阈值时写入即落该层。
//   * 容量上界（需求 2.4）：淘汰后占用 <= 配置容量；受害者按「频次 + 最近性」
//     选取，降级到下一较慢层，无较慢层时移除。
//   * NVMe 未配置时（需求 2.7）：HBM/DRAM 上的全部操作照常成功。
//
// 并发：内部以单一互斥量串行化共享层元数据（需求 7.2）；操作粒度为
// 单次 API 调用，观察者不会看到撕裂状态。
// =============================================================================
#ifndef PROJECT_STORAGE_TIERED_STORAGE_MANAGER_H_
#define PROJECT_STORAGE_TIERED_STORAGE_MANAGER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "project/status.h"
#include "project/types.h"

namespace project::storage {

// 存储层级：数值越小越快（kHBM 最快）。
enum class Tier : std::uint8_t { kHBM = 0, kDRAM = 1, kNVMe = 2 };

// 返回层级的稳定短名，供日志/诊断使用。
const char* TierName(Tier tier) noexcept;

// 各层容量配置。nullopt 表示该层未配置（如 NVMe 关闭，需求 2.7）。
struct TierConfig {
  std::optional<std::size_t> hbm_capacity_bytes;
  std::optional<std::size_t> dram_capacity_bytes;
  std::optional<std::size_t> nvme_capacity_bytes;  // nullopt => NVMe 禁用
  double high_water_ratio = 0.9;                   // 淘汰触发阈值
  // 多租户配额权重（非空即启用）：租户在每层的份额 =
  // 容量 × 权重/总权重。淘汰时超额最多的租户优先受害（组内纯 LRU）；
  // 无超额租户时回退全局策略（work-conserving：空闲份额可被借用）。
  // 未登记权重的租户份额为 0（最低优先级）。
  std::map<std::uint32_t, double> tenant_weights;
};

// 访问统计：频次 + 最近性（逻辑时钟），驱动晋升/淘汰（需求 2.3/2.4/2.6）。
struct AccessStats {
  std::uint64_t access_count = 0;
  std::uint64_t last_access_ts = 0;
};

// 层内元数据条目。
struct TierEntry {
  BlockKey key;
  Tier tier = Tier::kHBM;
  std::size_t size_bytes = 0;
  AccessStats stats;
  std::uint32_t tenant_id = 0;  // 多租户配额的归属租户（默认单租户 0）
};

// 层占用视图。
struct TierOccupancy {
  std::size_t used_bytes = 0;
  std::size_t capacity_bytes = 0;
};

// 定位结果：hit=false 表示缓存未命中（需求 2.5）。
struct LookupResult {
  bool hit = false;
  Tier tier = Tier::kHBM;
  BlockKey key;
};

class TieredStorageManager {
 public:
  explicit TieredStorageManager(TierConfig config);

  // 写入：目标层占用低于阈值时直接落层（需求 2.2）；达到/超过阈值先触发淘汰
  // （需求 2.4）。若块已在其他层，先移除旧放置以维持单层不变量（需求 2.1）。
  // 目标层未配置时落到最近的已配置较快层（无则较慢层）。
  // tenant_id 仅在配额启用（TierConfig::tenant_weights 非空）时参与淘汰亲和。
  Status Write(const KVCacheBlock& block, Tier target,
               std::uint32_t tenant_id = 0);

  // 读取：命中返回块并更新最近性+频次；较慢层命中记入晋升候选（需求 2.3）。
  // 未命中返回 kNotFound（需求 2.5）。
  Result<KVCacheBlock> Read(const BlockKey& key);

  // 定位：在存块解析到恰好一个权威层（需求 2.1）。
  LookupResult Locate(const BlockKey& key) const;

  // 容量执法：按分段 LRU + 频次加权选受害者，降级到下一较慢已配置层，
  // 无较慢层时移除；保证事后占用 <= 容量（需求 2.4）。
  Status EnforceCapacity(Tier tier);

  // 预取：把预测将复用的在存块晋升到最快已配置层，内容保持不变（需求 2.6）。
  Status Prefetch(const std::vector<BlockKey>& predicted);

  // 查询某层占用（未配置层返回 capacity=0）。
  TierOccupancy Occupancy(Tier tier) const;

  // 读取某块的访问统计（不在存返回 nullopt）；测试与调度器预取提示使用。
  std::optional<AccessStats> Stats(const BlockKey& key) const;

 private:
  // 以下私有方法均要求调用方已持有 mu_。
  std::optional<Tier> ResolveConfiguredTier(Tier target) const;
  std::optional<Tier> NextSlowerConfigured(Tier tier) const;
  std::size_t CapacityOf(Tier tier) const;
  std::size_t EvictionThresholdOf(Tier tier) const;
  Status EnforceCapacityLocked(Tier tier);
  Status PlaceLocked(KVCacheBlock block, Tier tier, std::uint32_t tenant_id);
  void RemoveLocked(const BlockKey& key);
  // 最快层 S3-FIFO 受害者选择（SOSP'23，与 bench.policies.S3FifoPolicy 对齐）：
  // 小队列超预算时先清小队列（复用过→晋升主队列，一次性→受害者+幽灵），
  // 否则主队列二次机会（freq>0 降频重入队尾，freq==0 受害）。
  std::optional<BlockKey> PickS3VictimLocked();
  // 较慢层纯 LRU 受害者选择（与 bench.policies.LRUPolicy / bench.tiered 的
  // slow=lru 胜出选型对齐）：受害者为最久未刷新最近性者。较慢层块的
  // last_access_ts 在「降级入层」时刷新为当前逻辑时钟（等价模型 slow.admit
  // 的 move_to_end→MRU），且命中即晋升离层、驻留期间不再被 touch，故
  // last_access_ts 恒等于降级顺序——纯 LRU 全序。基于有序受害者索引
  // （victim_index_）单次选取 O(1)。
  std::optional<BlockKey> PickVictimLocked(Tier tier) const;
  // 受害者索引维护：块入层/离层/访问统计变更时同步增删（O(log n)）。
  void IndexInsertLocked(const TierEntry& entry);
  void IndexEraseLocked(const TierEntry& entry);
  // S3-FIFO 幽灵队列（SOSP'23）：记录最近被彻底淘汰的冷块键；幽灵命中的
  // 新写入以「热」身份准入，避免复用块被一次性访问流量反复冲刷。
  void GhostInsertLocked(const BlockKey& key);
  bool GhostConsumeLocked(const BlockKey& key);
  // 多租户配额（tenant_weights 非空时启用）：加权 max-min 公平。
  bool QuotaEnabledLocked() const { return !config_.tenant_weights.empty(); }
  double TenantShareOf(std::uint32_t tenant, Tier tier) const;
  // 超额最多的租户的组内 LRU 受害者；无超额租户返回 nullopt。
  std::optional<BlockKey> PickQuotaVictimLocked(Tier tier) const;
  // 租户记账：块入层/离层/最近性变更时同步增删（仅配额启用时）。
  void AccountInsertLocked(const TierEntry& entry);
  void AccountEraseLocked(const TierEntry& entry);

  TierConfig config_;
  mutable std::mutex mu_;
  std::uint64_t logical_clock_ = 0;
  // 块数据 + 元数据；tier 字段为权威放置（单层不变量的落点）。
  // s3_freq / s3_in_main 仅对最快层块有效（S3-FIFO 队列归属与小频次计数）。
  struct StoredBlock {
    KVCacheBlock block;
    TierEntry entry;
    std::uint8_t s3_freq = 0;
    bool s3_in_main = false;
  };
  std::unordered_map<BlockKey, StoredBlock> blocks_;
  // 较慢层受害者有序索引：(最近性, 键) 升序，首元素即受害者（纯 LRU）。
  // 与 blocks_ 中的 TierEntry 严格同步（变更前先擦除旧序，变更后重插）；
  // 最快层改用 S3-FIFO 队列，不再使用本索引。
  using VictimOrder = std::tuple<std::uint64_t, BlockKey>;
  std::array<std::set<VictimOrder>, 3> victim_index_;
  // 最快层 S3-FIFO 队列（惰性失效：弹出时校验块仍在对应队列）。
  // 小队列预算 = 最快层容量的 10%（与模型一致）。
  std::deque<BlockKey> s3_small_fifo_;
  std::deque<BlockKey> s3_main_fifo_;
  std::size_t s3_small_used_bytes_ = 0;
  std::optional<Tier> fastest_tier_cache_;
  // 每层占用字节数。
  std::unordered_map<std::uint8_t, std::size_t> used_bytes_;
  // 幽灵队列（FIFO 定容）：仅存键，不存负载。
  // 定容 2× 块数当量：tiered 模式参数扫描下加倍幽灵队列在四条 trace 上
  // 稳定 +1% 吞吐（仅存键，内存成本可忽），与 bench.policies.S3FifoPolicy 对齐。
  static constexpr std::size_t kGhostCapacity = 8192;
  std::unordered_set<BlockKey> ghost_;
  std::deque<BlockKey> ghost_fifo_;
  // 多租户配额记账（仅 tenant_weights 非空时维护）：每层每租户占用字节数
  // 与组内 (最近性, 键) 有序索引（组内纯 LRU 受害者选取）。
  std::array<std::unordered_map<std::uint32_t, std::size_t>, 3> tenant_used_;
  std::array<std::map<std::uint32_t, std::set<VictimOrder>>, 3>
      tenant_victim_index_;
};

}  // namespace project::storage

#endif  // PROJECT_STORAGE_TIERED_STORAGE_MANAGER_H_
