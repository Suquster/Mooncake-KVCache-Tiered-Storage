// 分层存储管理器实现（设计依据见头文件与 design.md「Tiered_Storage_Manager」）。
#include "storage/tiered_storage_manager.h"

#include <algorithm>
#include <utility>

namespace project::storage {

namespace {

// 块的占用字节 = K + V 负载字节（元数据不计入容量核算）。
std::size_t BlockSizeBytes(const KVCacheBlock& block) {
  return block.k_payload.size() + block.v_payload.size();
}

// 由快到慢的层枚举序（Tier 底层值即为快慢序）。
constexpr Tier kTierOrder[] = {Tier::kHBM, Tier::kDRAM, Tier::kNVMe};

}  // namespace

const char* TierName(Tier tier) noexcept {
  switch (tier) {
    case Tier::kHBM:
      return "HBM";
    case Tier::kDRAM:
      return "DRAM";
    case Tier::kNVMe:
      return "NVMe";
  }
  return "unknown";
}

TieredStorageManager::TieredStorageManager(TierConfig config)
    : config_(std::move(config)) {
  fastest_tier_cache_ = ResolveConfiguredTier(Tier::kHBM);
}

std::size_t TieredStorageManager::CapacityOf(Tier tier) const {
  switch (tier) {
    case Tier::kHBM:
      return config_.hbm_capacity_bytes.value_or(0);
    case Tier::kDRAM:
      return config_.dram_capacity_bytes.value_or(0);
    case Tier::kNVMe:
      return config_.nvme_capacity_bytes.value_or(0);
  }
  return 0;
}

std::optional<Tier> TieredStorageManager::ResolveConfiguredTier(
    Tier target) const {
  auto configured = [this](Tier t) {
    switch (t) {
      case Tier::kHBM:
        return config_.hbm_capacity_bytes.has_value();
      case Tier::kDRAM:
        return config_.dram_capacity_bytes.has_value();
      case Tier::kNVMe:
        return config_.nvme_capacity_bytes.has_value();
    }
    return false;
  };
  if (configured(target)) {
    return target;
  }
  // 目标层未配置：先向较快层回退，再向较慢层回退（需求 2.7 的稳健化处理）。
  const int idx = static_cast<int>(target);
  for (int i = idx - 1; i >= 0; --i) {
    if (configured(kTierOrder[i])) {
      return kTierOrder[i];
    }
  }
  for (int i = idx + 1; i < 3; ++i) {
    if (configured(kTierOrder[i])) {
      return kTierOrder[i];
    }
  }
  return std::nullopt;
}

std::size_t TieredStorageManager::EvictionThresholdOf(Tier tier) const {
  // 淘汰目标水位：高水位（capacity * ratio）与容量的较小者。达到/超过高水位即
  // 触发淘汰（需求 2.4），事后占用 <= 目标水位 <= 容量。
  const std::size_t capacity = CapacityOf(tier);
  return static_cast<std::size_t>(std::min<double>(
      static_cast<double>(capacity),
      static_cast<double>(capacity) * config_.high_water_ratio));
}

std::optional<Tier> TieredStorageManager::NextSlowerConfigured(
    Tier tier) const {
  for (int i = static_cast<int>(tier) + 1; i < 3; ++i) {
    const Tier candidate = kTierOrder[i];
    const bool configured =
        (candidate == Tier::kHBM && config_.hbm_capacity_bytes) ||
        (candidate == Tier::kDRAM && config_.dram_capacity_bytes) ||
        (candidate == Tier::kNVMe && config_.nvme_capacity_bytes);
    if (configured) {
      return candidate;
    }
  }
  return std::nullopt;
}

void TieredStorageManager::RemoveLocked(const BlockKey& key) {
  auto it = blocks_.find(key);
  if (it == blocks_.end()) {
    return;
  }
  if (fastest_tier_cache_ && it->second.entry.tier == *fastest_tier_cache_) {
    if (!it->second.s3_in_main) {
      s3_small_used_bytes_ -= it->second.entry.size_bytes;
    }
    // 队列中的残留键在弹出时惰性校验清理。
  } else {
    IndexEraseLocked(it->second.entry);
  }
  AccountEraseLocked(it->second.entry);
  used_bytes_[static_cast<std::uint8_t>(it->second.entry.tier)] -=
      it->second.entry.size_bytes;
  blocks_.erase(it);
}

void TieredStorageManager::IndexInsertLocked(const TierEntry& entry) {
  victim_index_[static_cast<std::uint8_t>(entry.tier)].emplace(
      entry.stats.last_access_ts, entry.key);
}

void TieredStorageManager::IndexEraseLocked(const TierEntry& entry) {
  victim_index_[static_cast<std::uint8_t>(entry.tier)].erase(
      VictimOrder{entry.stats.last_access_ts, entry.key});
}

std::optional<BlockKey> TieredStorageManager::PickS3VictimLocked() {
  // S3-FIFO（SOSP'23）：小队列超预算（容量 10%）或主队列空时先清小队列：
  // 复用过（freq>0）晋升主队列，一次性→受害者（记入幽灵）；否则主队列
  // 二次机会：freq>0 降频重入队尾，freq==0 受害。队列元素惰性失效。
  const Tier fast = *fastest_tier_cache_;
  const std::size_t small_budget = CapacityOf(fast) / 10;
  while (true) {
    const bool from_small = (s3_small_used_bytes_ > small_budget &&
                             !s3_small_fifo_.empty()) ||
                            s3_main_fifo_.empty();
    if (from_small) {
      if (s3_small_fifo_.empty()) {
        return std::nullopt;
      }
      const BlockKey key = s3_small_fifo_.front();
      s3_small_fifo_.pop_front();
      auto it = blocks_.find(key);
      if (it == blocks_.end() || it->second.entry.tier != fast ||
          it->second.s3_in_main) {
        continue;  // 残留键：惰性清理。
      }
      if (it->second.s3_freq > 0) {
        it->second.s3_in_main = true;  // 复用过 → 晋升主队列。
        it->second.s3_freq = 0;
        s3_small_used_bytes_ -= it->second.entry.size_bytes;
        s3_main_fifo_.push_back(key);
        continue;
      }
      GhostInsertLocked(key);  // 一次性访问 → 幽灵队列。
      return key;
    }
    const BlockKey key = s3_main_fifo_.front();
    s3_main_fifo_.pop_front();
    auto it = blocks_.find(key);
    if (it == blocks_.end() || it->second.entry.tier != fast ||
        !it->second.s3_in_main) {
      continue;  // 残留键：惰性清理。
    }
    if (it->second.s3_freq > 0) {
      it->second.s3_freq -= 1;  // 二次机会：降频重入队尾。
      s3_main_fifo_.push_back(key);
      continue;
    }
    return key;
  }
}

std::optional<BlockKey> TieredStorageManager::PickVictimLocked(
    Tier tier) const {
  // 纯 LRU：受害者为最近性最小（最久未刷新）者。有序索引 (最近性, 键)
  // 升序与此全序一致，首元素即受害者。
  const auto& index = victim_index_[static_cast<std::uint8_t>(tier)];
  if (index.empty()) {
    return std::nullopt;
  }
  return std::get<1>(*index.begin());
}

void TieredStorageManager::AccountInsertLocked(const TierEntry& entry) {
  if (!QuotaEnabledLocked()) {
    return;
  }
  const auto t = static_cast<std::uint8_t>(entry.tier);
  tenant_used_[t][entry.tenant_id] += entry.size_bytes;
  tenant_victim_index_[t][entry.tenant_id].emplace(entry.stats.last_access_ts,
                                                   entry.key);
}

void TieredStorageManager::AccountEraseLocked(const TierEntry& entry) {
  if (!QuotaEnabledLocked()) {
    return;
  }
  const auto t = static_cast<std::uint8_t>(entry.tier);
  if (auto used_it = tenant_used_[t].find(entry.tenant_id);
      used_it != tenant_used_[t].end()) {
    used_it->second -= std::min(used_it->second, entry.size_bytes);
    if (used_it->second == 0) {
      tenant_used_[t].erase(used_it);
    }
  }
  if (auto idx_it = tenant_victim_index_[t].find(entry.tenant_id);
      idx_it != tenant_victim_index_[t].end()) {
    idx_it->second.erase(VictimOrder{entry.stats.last_access_ts, entry.key});
    if (idx_it->second.empty()) {
      tenant_victim_index_[t].erase(idx_it);
    }
  }
}

double TieredStorageManager::TenantShareOf(std::uint32_t tenant,
                                           Tier tier) const {
  double total = 0.0;
  for (const auto& [id, weight] : config_.tenant_weights) {
    total += weight;
  }
  if (total <= 0.0) {
    return 0.0;
  }
  const auto it = config_.tenant_weights.find(tenant);
  const double weight = it == config_.tenant_weights.end() ? 0.0 : it->second;
  return static_cast<double>(CapacityOf(tier)) * weight / total;
}

std::optional<BlockKey> TieredStorageManager::PickQuotaVictimLocked(
    Tier tier) const {
  // 加权 max-min 公平：超出自身份额最多的租户优先受害（组内纯 LRU）。
  // 无超额租户时返回 nullopt，回退全局策略（work-conserving 借用）。
  const auto t = static_cast<std::uint8_t>(tier);
  double worst_overage = 0.0;
  const std::set<VictimOrder>* worst_index = nullptr;
  for (const auto& [tenant, index] : tenant_victim_index_[t]) {
    if (index.empty()) {
      continue;
    }
    const auto used_it = tenant_used_[t].find(tenant);
    const double used = used_it == tenant_used_[t].end()
                            ? 0.0
                            : static_cast<double>(used_it->second);
    const double overage = used - TenantShareOf(tenant, tier);
    if (overage > worst_overage) {
      worst_overage = overage;
      worst_index = &index;
    }
  }
  if (worst_index == nullptr) {
    return std::nullopt;
  }
  return std::get<1>(*worst_index->begin());
}

void TieredStorageManager::GhostInsertLocked(const BlockKey& key) {
  if (ghost_.insert(key).second) {
    ghost_fifo_.push_back(key);
    while (ghost_fifo_.size() > kGhostCapacity) {
      ghost_.erase(ghost_fifo_.front());
      ghost_fifo_.pop_front();
    }
  }
}

bool TieredStorageManager::GhostConsumeLocked(const BlockKey& key) {
  auto it = ghost_.find(key);
  if (it == ghost_.end()) {
    return false;
  }
  ghost_.erase(it);  // FIFO 中的残留键在滚出时惰性清理。
  return true;
}

Status TieredStorageManager::EnforceCapacityLocked(Tier tier) {
  const std::size_t threshold = EvictionThresholdOf(tier);
  auto& used = used_bytes_[static_cast<std::uint8_t>(tier)];
  while (used > threshold) {
    const bool is_fastest = fastest_tier_cache_ && tier == *fastest_tier_cache_;
    // 配额启用时超额租户优先受害；无超额租户则回退全局策略。
    std::optional<BlockKey> victim_key;
    if (QuotaEnabledLocked()) {
      victim_key = PickQuotaVictimLocked(tier);
    }
    if (!victim_key) {
      victim_key = is_fastest ? PickS3VictimLocked() : PickVictimLocked(tier);
    }
    if (!victim_key) {
      return Status::Make(StatusCode::kInternal,
                          "占用超容量但层内无可淘汰块");
    }
    auto it = blocks_.find(*victim_key);
    KVCacheBlock victim_block = std::move(it->second.block);
    AccessStats victim_stats = it->second.entry.stats;
    const std::uint32_t victim_tenant = it->second.entry.tenant_id;
    RemoveLocked(*victim_key);
    if (const auto slower = NextSlowerConfigured(tier)) {
      // 降级：刷新最近性为当前逻辑时钟（等价模型 slow.admit 的
      // move_to_end→MRU），使较慢层纯 LRU 序按「降级顺序」排列；
      // 递归对较慢层执法。
      victim_stats.last_access_ts = ++logical_clock_;
      const std::size_t size = BlockSizeBytes(victim_block);
      if (size <= CapacityOf(*slower)) {
        const BlockKey key = victim_block.key;
        const TierEntry entry{key, *slower, size, victim_stats,
                              victim_tenant};
        blocks_.insert_or_assign(
            key, StoredBlock{std::move(victim_block), entry});
        IndexInsertLocked(entry);
        AccountInsertLocked(entry);
        used_bytes_[static_cast<std::uint8_t>(*slower)] += size;
        if (Status status = EnforceCapacityLocked(*slower); !status.ok()) {
          return status;
        }
      }
      // 装不下较慢层：直接移除（幽灵记账仅在最快层小队列淘汰时，
      // 与 bench.policies.S3FifoPolicy 对齐）。
    }
  }
  return Status::Ok();
}

Status TieredStorageManager::PlaceLocked(KVCacheBlock block, Tier tier,
                                         std::uint32_t tenant_id) {
  const std::size_t size = BlockSizeBytes(block);
  if (size > EvictionThresholdOf(tier)) {
    return Status::Make(StatusCode::kOutOfCapacity,
                        std::string("块尺寸超过层容量：") + TierName(tier));
  }
  // 单层不变量：先移除任何既有放置。
  AccessStats stats;
  if (auto it = blocks_.find(block.key); it != blocks_.end()) {
    stats = it->second.entry.stats;
    RemoveLocked(block.key);
  }
  stats.last_access_ts = ++logical_clock_;
  stats.access_count += 1;
  const bool ghost_hit = GhostConsumeLocked(block.key);
  if (stats.access_count == 1 && ghost_hit) {
    stats.access_count = 2;  // 幽灵命中：以热身份准入，免遭冷段优先淘汰。
  }
  const BlockKey key = block.key;
  const TierEntry entry{key, tier, size, stats, tenant_id};
  const bool is_fastest = fastest_tier_cache_ && tier == *fastest_tier_cache_;
  StoredBlock stored{std::move(block), entry};
  if (is_fastest) {
    // S3-FIFO 准入：幽灵命中或历史复用过 → 主队列；否则小队列。
    stored.s3_in_main = ghost_hit || stats.access_count > 1;
    stored.s3_freq = 0;
    if (stored.s3_in_main) {
      s3_main_fifo_.push_back(key);
    } else {
      s3_small_fifo_.push_back(key);
      s3_small_used_bytes_ += size;
    }
  }
  blocks_.insert_or_assign(key, std::move(stored));
  if (!is_fastest) {
    IndexInsertLocked(entry);
  }
  AccountInsertLocked(entry);
  used_bytes_[static_cast<std::uint8_t>(tier)] += size;
  return EnforceCapacityLocked(tier);
}

Status TieredStorageManager::Write(const KVCacheBlock& block, Tier target,
                                   std::uint32_t tenant_id) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto tier = ResolveConfiguredTier(target);
  if (!tier) {
    return Status::Make(StatusCode::kInvalidArgument, "无任何已配置存储层");
  }
  return PlaceLocked(block, *tier, tenant_id);
}

Result<KVCacheBlock> TieredStorageManager::Read(const BlockKey& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.find(key);
  if (it == blocks_.end()) {
    return Result<KVCacheBlock>(
        Status::Make(StatusCode::kNotFound, "块不在任何层（缓存未命中）"));
  }
  // 需求 2.3：命中即更新最近性与频次（较慢层命中的块由此获得晋升信号）。
  const bool is_fastest =
      fastest_tier_cache_ && it->second.entry.tier == *fastest_tier_cache_;
  if (is_fastest) {
    it->second.s3_freq =
        static_cast<std::uint8_t>(std::min<int>(it->second.s3_freq + 1, 3));
    AccountEraseLocked(it->second.entry);
    it->second.entry.stats.access_count += 1;
    it->second.entry.stats.last_access_ts = ++logical_clock_;
    AccountInsertLocked(it->second.entry);
  } else {
    IndexEraseLocked(it->second.entry);
    AccountEraseLocked(it->second.entry);
    it->second.entry.stats.access_count += 1;
    it->second.entry.stats.last_access_ts = ++logical_clock_;
    IndexInsertLocked(it->second.entry);
    AccountInsertLocked(it->second.entry);
    // 较慢层命中：立即晋升到最快层（与回放模型对齐；「读穿不晋升」为负结果，
    // 见基准报告），以复用身份入 S3-FIFO 主队列。
    if (fastest_tier_cache_ &&
        it->second.entry.size_bytes <=
            EvictionThresholdOf(*fastest_tier_cache_)) {
      KVCacheBlock block = it->second.block;
      const AccessStats stats = it->second.entry.stats;
      const std::size_t size = it->second.entry.size_bytes;
      const std::uint32_t tenant = it->second.entry.tenant_id;
      RemoveLocked(key);
      const Tier fast = *fastest_tier_cache_;
      const TierEntry entry{key, fast, size, stats, tenant};
      StoredBlock promoted{std::move(block), entry};
      promoted.s3_in_main = true;
      s3_main_fifo_.push_back(key);
      blocks_.insert_or_assign(key, std::move(promoted));
      AccountInsertLocked(entry);
      used_bytes_[static_cast<std::uint8_t>(fast)] += size;
      if (Status status = EnforceCapacityLocked(fast); !status.ok()) {
        return Result<KVCacheBlock>(status);
      }
      auto promoted_it = blocks_.find(key);
      if (promoted_it == blocks_.end()) {
        return Result<KVCacheBlock>(Status::Make(
            StatusCode::kInternal, "晋升块在容量执法中被立即淘汰"));
      }
      return Result<KVCacheBlock>(promoted_it->second.block);
    }
  }
  return Result<KVCacheBlock>(it->second.block);
}

LookupResult TieredStorageManager::Locate(const BlockKey& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.find(key);
  if (it == blocks_.end()) {
    return LookupResult{false, Tier::kHBM, key};
  }
  return LookupResult{true, it->second.entry.tier, key};
}

Status TieredStorageManager::EnforceCapacity(Tier tier) {
  std::lock_guard<std::mutex> lock(mu_);
  return EnforceCapacityLocked(tier);
}

Status TieredStorageManager::Prefetch(const std::vector<BlockKey>& predicted) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto fastest = ResolveConfiguredTier(Tier::kHBM);
  if (!fastest) {
    return Status::Make(StatusCode::kInvalidArgument, "无任何已配置存储层");
  }
  for (const BlockKey& key : predicted) {
    auto it = blocks_.find(key);
    if (it == blocks_.end() || it->second.entry.tier == *fastest) {
      continue;  // 不在存或已在最快层：跳过。
    }
    if (BlockSizeBytes(it->second.block) > EvictionThresholdOf(*fastest)) {
      continue;  // 装不进最快层：保持原层（内容不丢）。
    }
    KVCacheBlock block = it->second.block;
    const std::uint32_t tenant = it->second.entry.tenant_id;
    if (Status status = PlaceLocked(std::move(block), *fastest, tenant);
        !status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

TierOccupancy TieredStorageManager::Occupancy(Tier tier) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = used_bytes_.find(static_cast<std::uint8_t>(tier));
  return TierOccupancy{it == used_bytes_.end() ? 0 : it->second,
                       CapacityOf(tier)};
}

std::optional<AccessStats> TieredStorageManager::Stats(
    const BlockKey& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.find(key);
  if (it == blocks_.end()) {
    return std::nullopt;
  }
  return it->second.entry.stats;
}

}  // namespace project::storage
