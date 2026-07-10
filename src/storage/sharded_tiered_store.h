// =============================================================================
// src/storage/sharded_tiered_store.h —— 分片并发分层存储（性能优化）
// =============================================================================
// 动机：TieredStorageManager 以单互斥量串行化全部操作，在多核高并发下互斥量
// 成为吞吐瓶颈。本包装把键空间按 hash 划分到 N 个独立分片（每片一个
// TieredStorageManager + 独立锁），不同分片上的操作完全并行。
//
// 正确性论证：
//   * 每个键恒定映射到唯一分片 ⇒ 单层放置/容量上界等不变量在分片内成立，
//     全局不变量为分片不变量的合取（键空间不相交）。
//   * 每分片容量 = 全局容量 / N ⇒ 全局占用 <= 全局容量恒成立。
//   * 对外 API 与 TieredStorageManager 逐一同名同义，可直接替换。
// =============================================================================
#ifndef PROJECT_STORAGE_SHARDED_TIERED_STORE_H_
#define PROJECT_STORAGE_SHARDED_TIERED_STORE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "storage/tiered_storage_manager.h"

namespace project::storage {

class ShardedTieredStore {
 public:
  // 全局容量按分片数均分；shard_count 建议为 2 的幂（默认 16）。
  explicit ShardedTieredStore(TierConfig global_config,
                              std::size_t shard_count = 16)
      : shard_count_(shard_count == 0 ? 1 : shard_count) {
    TierConfig per_shard = global_config;
    const auto divide = [this](std::optional<std::size_t>& capacity) {
      if (capacity.has_value()) {
        *capacity = *capacity / shard_count_;
      }
    };
    divide(per_shard.hbm_capacity_bytes);
    divide(per_shard.dram_capacity_bytes);
    divide(per_shard.nvme_capacity_bytes);
    shards_.reserve(shard_count_);
    for (std::size_t i = 0; i < shard_count_; ++i) {
      shards_.push_back(std::make_unique<TieredStorageManager>(per_shard));
    }
  }

  Status Write(const KVCacheBlock& block, Tier target) {
    return ShardOf(block.key).Write(block, target);
  }

  Result<KVCacheBlock> Read(const BlockKey& key) {
    return ShardOf(key).Read(key);
  }

  LookupResult Locate(const BlockKey& key) { return ShardOf(key).Locate(key); }

  Status Prefetch(const std::vector<BlockKey>& keys) {
    for (const BlockKey& key : keys) {
      if (const auto status = ShardOf(key).Prefetch({key}); !status.ok()) {
        return status;
      }
    }
    return Status::Ok();
  }

  Status EnforceCapacity(Tier tier) {
    for (const auto& shard : shards_) {
      if (const auto status = shard->EnforceCapacity(tier); !status.ok()) {
        return status;
      }
    }
    return Status::Ok();
  }

  // 全局占用 = 各分片占用之和。
  TierOccupancy Occupancy(Tier tier) const {
    TierOccupancy total{0, 0};
    for (const auto& shard : shards_) {
      const auto occupancy = shard->Occupancy(tier);
      total.used_bytes += occupancy.used_bytes;
      total.capacity_bytes += occupancy.capacity_bytes;
    }
    return total;
  }

  std::size_t shard_count() const noexcept { return shard_count_; }

 private:
  TieredStorageManager& ShardOf(const BlockKey& key) {
    return *shards_[std::hash<BlockKey>{}(key) % shard_count_];
  }

  std::size_t shard_count_;
  std::vector<std::unique_ptr<TieredStorageManager>> shards_;
};

}  // namespace project::storage

#endif  // PROJECT_STORAGE_SHARDED_TIERED_STORE_H_
