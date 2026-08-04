// 分层存储属性测试（RapidCheck，最少 100 次迭代，Properties 1–5、19–20）。
// 标签格式：Feature: mooncake-kvcache-optimization, Property {n}: {text}
#include <rapidcheck.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "storage/prefix_prefetcher.h"
#include "storage/tiered_storage_manager.h"

using project::BlockKey;
using project::DType;
using project::KVCacheBlock;
using project::storage::Tier;
using project::storage::TierConfig;
using project::storage::TieredStorageManager;

namespace {

// 生成固定负载尺寸的测试块（尺寸小以便驱动淘汰路径）。
KVCacheBlock MakeBlock(std::uint64_t hash_id, std::size_t payload = 8) {
  KVCacheBlock block;
  block.key = BlockKey{hash_id, 0};
  block.num_tokens = 1;
  block.num_heads = 1;
  block.head_dim = 1;
  block.dtype = DType::kFloat16;
  block.k_payload.assign(payload / 2, std::byte{0xAB});
  block.v_payload.assign(payload - payload / 2, std::byte{0xCD});
  return block;
}

TierConfig SmallConfig() {
  TierConfig config;
  config.hbm_capacity_bytes = 64;
  config.dram_capacity_bytes = 128;
  config.nvme_capacity_bytes = 256;
  config.high_water_ratio = 1.0;  // 阈值 == 容量，便于精确断言容量上界
  return config;
}

// 随机操作序列驱动器：write/read/prefetch/enforce。
struct Op {
  int kind;               // 0=write 1=read 2=prefetch 3=enforce
  std::uint64_t hash_id;  // 作用键
  int tier;               // write 目标层 / enforce 层
};

}  // namespace

int main() {
  bool ok = true;

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 1: "
      "single-tier placement invariant",
      [] {
        TieredStorageManager mgr(SmallConfig());
        const auto ops = *rc::gen::container<std::vector<Op>>(
            rc::gen::build<Op>(
                rc::gen::set(&Op::kind, rc::gen::inRange(0, 4)),
                rc::gen::set(&Op::hash_id,
                             rc::gen::inRange<std::uint64_t>(0, 12)),
                rc::gen::set(&Op::tier, rc::gen::inRange(0, 3))));
        for (const Op& op : ops) {
          const BlockKey key{op.hash_id, 0};
          switch (op.kind) {
            case 0:
              (void)mgr.Write(MakeBlock(op.hash_id),
                              static_cast<Tier>(op.tier));
              break;
            case 1:
              (void)mgr.Read(key);
              break;
            case 2:
              (void)mgr.Prefetch({key});
              break;
            default:
              (void)mgr.EnforceCapacity(static_cast<Tier>(op.tier));
          }
          // 不变量：每个曾写入的键要么未命中，要么解析到恰好一个权威层，
          // 且重复 Locate 结果一致（元数据一致性）。
          for (std::uint64_t h = 0; h < 12; ++h) {
            const auto first = mgr.Locate(BlockKey{h, 0});
            const auto second = mgr.Locate(BlockKey{h, 0});
            RC_ASSERT(first.hit == second.hit);
            if (first.hit) {
              RC_ASSERT(first.tier == second.tier);
            }
          }
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 2: "
      "threshold-respecting placement",
      [] {
        TieredStorageManager mgr(SmallConfig());
        const auto tier = static_cast<Tier>(*rc::gen::inRange(0, 3));
        const auto hash_id = *rc::gen::inRange<std::uint64_t>(0, 1000);
        // 空层占用为 0，必然低于阈值：写入即落该层。
        RC_ASSERT(mgr.Write(MakeBlock(hash_id), tier).ok());
        const auto located = mgr.Locate(BlockKey{hash_id, 0});
        RC_ASSERT(located.hit);
        RC_ASSERT(located.tier == tier);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 3: "
      "read records access signals and enables promotion",
      [] {
        TieredStorageManager mgr(SmallConfig());
        const auto hash_id = *rc::gen::inRange<std::uint64_t>(0, 1000);
        const auto reads = *rc::gen::inRange<int>(1, 8);
        const KVCacheBlock block = MakeBlock(hash_id);
        RC_ASSERT(mgr.Write(block, Tier::kDRAM).ok());
        const auto before = mgr.Stats(block.key);
        RC_ASSERT(before.has_value());
        for (int i = 0; i < reads; ++i) {
          RC_ASSERT(mgr.Read(block.key).ok());
        }
        const auto after = mgr.Stats(block.key);
        RC_ASSERT(after.has_value());
        // 频次与最近性均被读更新（需求 2.3）。
        RC_ASSERT(after->access_count ==
                  before->access_count + static_cast<std::uint64_t>(reads));
        RC_ASSERT(after->last_access_ts > before->last_access_ts);
        // 访问信号触发预取晋升：内容保持、层变最快（需求 2.6）。
        RC_ASSERT(mgr.Prefetch({block.key}).ok());
        const auto located = mgr.Locate(block.key);
        RC_ASSERT(located.hit);
        RC_ASSERT(located.tier == Tier::kHBM);
        const auto promoted = mgr.Read(block.key);
        RC_ASSERT(promoted.ok());
        RC_ASSERT(promoted.value() == block);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 4: "
      "eviction preserves the capacity bound",
      [] {
        TieredStorageManager mgr(SmallConfig());
        const auto count = *rc::gen::inRange<int>(1, 64);
        for (int i = 0; i < count; ++i) {
          RC_ASSERT(
              mgr.Write(MakeBlock(static_cast<std::uint64_t>(i)), Tier::kHBM)
                  .ok());
        }
        for (const Tier tier : {Tier::kHBM, Tier::kDRAM, Tier::kNVMe}) {
          const auto occupancy = mgr.Occupancy(tier);
          RC_ASSERT(occupancy.used_bytes <= occupancy.capacity_bytes);
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 5: "
      "cache miss for absent blocks; NVMe-disabled operation",
      [] {
        // NVMe 未配置（需求 2.7）。
        TierConfig config;
        config.hbm_capacity_bytes = 64;
        config.dram_capacity_bytes = 128;
        config.nvme_capacity_bytes = std::nullopt;
        config.high_water_ratio = 1.0;
        TieredStorageManager mgr(config);

        const auto absent = *rc::gen::inRange<std::uint64_t>(1000, 2000);
        // 不在任何层的键：读报未命中（需求 2.5）。
        RC_ASSERT(!mgr.Locate(BlockKey{absent, 0}).hit);
        RC_ASSERT(mgr.Read(BlockKey{absent, 0}).status().code() ==
                  project::StatusCode::kNotFound);

        // HBM/DRAM 全操作照常成功（需求 2.7）。
        const auto hash_id = *rc::gen::inRange<std::uint64_t>(0, 100);
        const KVCacheBlock block = MakeBlock(hash_id);
        RC_ASSERT(mgr.Write(block, Tier::kHBM).ok());
        RC_ASSERT(mgr.Write(block, Tier::kDRAM).ok());
        RC_ASSERT(mgr.Read(block.key).ok());
        RC_ASSERT(mgr.Prefetch({block.key}).ok());
        RC_ASSERT(mgr.EnforceCapacity(Tier::kHBM).ok());
        RC_ASSERT(mgr.EnforceCapacity(Tier::kDRAM).ok());
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 19: "
      "prefix prefetcher predicts recorded successor chains in order",
      [] {
        const auto count = *rc::gen::inRange<int>(2, 32);
        std::vector<BlockKey> seq;
        seq.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
          seq.push_back(BlockKey{static_cast<std::uint64_t>(i), 0});
        }
        project::storage::PrefixPrefetcher prefetcher;
        prefetcher.RecordSequence(seq);
        RC_ASSERT(prefetcher.EdgeCount() == seq.size() - 1);

        const auto budget = *rc::gen::inRange<std::size_t>(0, 64);
        const auto chain = prefetcher.PredictChain(seq.front(), budget);
        const std::size_t expected = std::min(budget, seq.size() - 1);
        RC_ASSERT(chain.size() == expected);
        for (std::size_t i = 0; i < chain.size(); ++i) {
          RC_ASSERT(chain[i] == seq[i + 1]);
        }
        // 未记录的键无预测；单元素序列不产生边。
        RC_ASSERT(
            prefetcher.PredictChain(BlockKey{9999999, 0}, budget).empty());
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 20: "
      "prefix prefetcher is bounded and cycle-safe",
      [] {
        const auto cap = *rc::gen::inRange<std::size_t>(1, 16);
        project::storage::PrefixPrefetcher prefetcher(cap);
        const auto total = *rc::gen::inRange<int>(2, 64);
        std::vector<BlockKey> seq;
        for (int i = 0; i < total; ++i) {
          seq.push_back(BlockKey{static_cast<std::uint64_t>(i), 0});
        }
        prefetcher.RecordSequence(seq);
        RC_ASSERT(prefetcher.EdgeCount() <= cap);

        // 环序列：a→b→a；预测在绕回锚点时终止，不会死循环。
        project::storage::PrefixPrefetcher cyclic;
        const BlockKey a{1, 0};
        const BlockKey b{2, 0};
        cyclic.RecordSequence({a, b, a});
        const auto chain = cyclic.PredictChain(a, 100);
        RC_ASSERT(chain.size() == std::size_t{1});
        RC_ASSERT(chain.front() == b);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 21: "
      "multi-anchor prediction is deduplicated union of per-anchor chains",
      [] {
        const auto count = *rc::gen::inRange<int>(2, 32);
        std::vector<BlockKey> seq;
        seq.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
          seq.push_back(BlockKey{static_cast<std::uint64_t>(i), 0});
        }
        project::storage::PrefixPrefetcher prefetcher;
        prefetcher.RecordSequence(seq);

        const auto budget = *rc::gen::inRange<std::size_t>(1, 64);
        const auto multi = prefetcher.PredictChainMulti(seq, budget);
        // 去重并集 = 各锚点单链预测的合并（无重复）。
        std::vector<BlockKey> expected;
        for (const auto& anchor : seq) {
          for (const auto& key : prefetcher.PredictChain(anchor, budget)) {
            if (std::find(expected.begin(), expected.end(), key) ==
                expected.end()) {
              expected.push_back(key);
            }
          }
        }
        RC_ASSERT(multi == expected);
        // 环序列不死循环；锚点自身不在其自身链中，但可被其他锚点链覆盖。
        project::storage::PrefixPrefetcher cyclic;
        const BlockKey a{1, 0};
        const BlockKey b{2, 0};
        cyclic.RecordSequence({a, b, a});
        const auto chain = cyclic.PredictChainMulti({a, b}, 100);
        RC_ASSERT(chain.size() == std::size_t{2});
        RC_ASSERT(chain[0] == b);
        RC_ASSERT(chain[1] == a);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 22: "
      "slower-tier eviction is pure LRU by admission order",
      [] {
        // 仅 HBM + DRAM（DRAM 为终端较慢层，淘汰即移除）；DRAM 容量恰好
        // 4 个 8 字节块。直接按序写入 DRAM，验证纯 LRU：最早写入者最先被逐。
        TierConfig config;
        config.hbm_capacity_bytes = 64;
        config.dram_capacity_bytes = 32;  // 4 × 8 字节
        config.nvme_capacity_bytes = std::nullopt;
        config.high_water_ratio = 1.0;
        TieredStorageManager mgr(config);

        for (std::uint64_t i = 0; i < 4; ++i) {
          RC_ASSERT(mgr.Write(MakeBlock(i), Tier::kDRAM).ok());
        }
        // 满载后再写入两块，触发两次淘汰。
        RC_ASSERT(mgr.Write(MakeBlock(4), Tier::kDRAM).ok());  // 逐出最旧 id0
        RC_ASSERT(mgr.Write(MakeBlock(5), Tier::kDRAM).ok());  // 逐出次旧 id1

        // 纯 LRU：最早写入的 id0/id1 被逐（无较慢层，彻底移除）；
        // 较新的 id2..id5 全部驻留 DRAM。
        RC_ASSERT(!mgr.Locate(BlockKey{0, 0}).hit);
        RC_ASSERT(!mgr.Locate(BlockKey{1, 0}).hit);
        for (std::uint64_t i = 2; i < 6; ++i) {
          const auto located = mgr.Locate(BlockKey{i, 0});
          RC_ASSERT(located.hit);
          RC_ASSERT(located.tier == Tier::kDRAM);
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 23: "
      "tenant quota shields under-share tenant from flooding neighbor",
      [] {
        // \u5355\u5c42 HBM 64B = 8 \u4e2a 8B \u5757\uff1b\u4e24\u79df\u6237\u5404\u5360 50% \u4efd\u989d\uff084 \u5757\uff09\u3002
        // \u79df\u6237 1 \u5148\u5199\u6ee1\u81ea\u8eab\u4efd\u989d\uff0c\u79df\u6237 2 \u968f\u540e\u6d2a\u6c34\u5f0f\u5199\u5165\uff1a\u914d\u989d\u6dd8\u6c70\u5e94\u53ea
        // \u727a\u7272\u8d85\u989d\u7684\u79df\u6237 2\uff08\u7ec4\u5185\u7eaf LRU\uff09\uff0c\u79df\u6237 1 \u7684\u5757\u5168\u90e8\u5b58\u6d3b\u3002
        TierConfig config;
        config.hbm_capacity_bytes = 64;
        config.dram_capacity_bytes = std::nullopt;
        config.nvme_capacity_bytes = std::nullopt;
        config.high_water_ratio = 1.0;
        config.tenant_weights = {{1, 0.5}, {2, 0.5}};
        TieredStorageManager mgr(config);

        for (std::uint64_t i = 0; i < 4; ++i) {  // \u79df\u6237 1\uff1a\u6070\u597d\u5360\u6ee1\u4efd\u989d\u3002
          RC_ASSERT(mgr.Write(MakeBlock(i), Tier::kHBM, 1).ok());
        }
        for (std::uint64_t i = 100; i < 106; ++i) {  // \u79df\u6237 2\uff1a\u6d2a\u6c34 6 \u5757\u3002
          RC_ASSERT(mgr.Write(MakeBlock(i), Tier::kHBM, 2).ok());
        }

        // \u79df\u6237 1 \u672a\u8d85\u989d\uff1a\u5168\u90e8\u5b58\u6d3b\uff1b\u79df\u6237 2 \u8d85\u989d\uff1a\u6700\u65e9\u4e24\u5757\u88ab\u9010\uff0c
        // \u7559\u5b58\u6700\u65b0 4 \u5757\uff08\u6070\u4e3a\u5176\u4efd\u989d\uff09\u3002
        for (std::uint64_t i = 0; i < 4; ++i) {
          RC_ASSERT(mgr.Locate(BlockKey{i, 0}).hit);
        }
        RC_ASSERT(!mgr.Locate(BlockKey{100, 0}).hit);
        RC_ASSERT(!mgr.Locate(BlockKey{101, 0}).hit);
        for (std::uint64_t i = 102; i < 106; ++i) {
          RC_ASSERT(mgr.Locate(BlockKey{i, 0}).hit);
        }
      });

  return ok ? 0 : 1;
}
