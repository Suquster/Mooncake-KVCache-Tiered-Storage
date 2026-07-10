// =============================================================================
// src/storage/prefix_prefetcher.h —— 前缀后继图预取预测器
// =============================================================================
// 设计依据：design.md「Prefetch_Engine」与 bench/tiered.py 的前缀感知预取
// （经四条 FAST'25 trace 验证：synthetic slow 命中 −93%，见基准报告）。
//
// 机制：记录请求内 KVCache_Block 键的相邻关系（块后继图）。请求到达时以块
// 序列中的在存块为锚点沿后继链预测将顺序访问的块，交由
// TieredStorageManager::Prefetch 提前晋升到最快层——层间搬运与未命中块的
// prefill 重叠（需求 2.6）。多锚点选型胜出（四 trace 吞吐再 +1.6%，见基准
// 报告）。
//
// 并发：内部互斥量串行化后继图读写（需求 7.2）。容量定界：FIFO 滚出最旧边，
// 防止长期运行下的无界增长。
// =============================================================================
#ifndef PROJECT_STORAGE_PREFIX_PREFETCHER_H_
#define PROJECT_STORAGE_PREFIX_PREFETCHER_H_

#include <cstddef>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "project/types.h"

namespace project::storage {

class PrefixPrefetcher {
 public:
  explicit PrefixPrefetcher(std::size_t max_edges = kDefaultMaxEdges);

  // 记录一个请求的块键序列：相邻键构成后继边（幂等，后写覆盖）。
  void RecordSequence(const std::vector<BlockKey>& keys);

  // 从 head 沿后继链预测至多 budget 个后续块键（不含 head 本身）。
  std::vector<BlockKey> PredictChain(const BlockKey& head,
                                     std::size_t budget) const;

  // 多锚点预测：以 anchors 中每个键为锚点各自沿后继链预测，合并去重
  // （不含锚点本身）；单次加锁遍历。
  std::vector<BlockKey> PredictChainMulti(const std::vector<BlockKey>& anchors,
                                          std::size_t budget) const;

  // 当前后继边数（测试/诊断）。
  std::size_t EdgeCount() const;

  static constexpr std::size_t kDefaultMaxEdges = 1 << 16;

 private:
  void InsertEdgeLocked(const BlockKey& from, const BlockKey& to);

  const std::size_t max_edges_;
  mutable std::mutex mu_;
  std::unordered_map<BlockKey, BlockKey> successor_;
  std::deque<BlockKey> fifo_;  // 边的插入序（按 from 键），用于定容滚出
};

}  // namespace project::storage

#endif  // PROJECT_STORAGE_PREFIX_PREFETCHER_H_
