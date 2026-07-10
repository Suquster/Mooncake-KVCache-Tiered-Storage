#include "storage/prefix_prefetcher.h"

#include <unordered_set>

namespace project::storage {

PrefixPrefetcher::PrefixPrefetcher(std::size_t max_edges)
    : max_edges_(max_edges == 0 ? 1 : max_edges) {}

void PrefixPrefetcher::RecordSequence(const std::vector<BlockKey>& keys) {
  if (keys.size() < 2) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
    InsertEdgeLocked(keys[i], keys[i + 1]);
  }
}

std::vector<BlockKey> PrefixPrefetcher::PredictChain(const BlockKey& head,
                                                     std::size_t budget) const {
  std::vector<BlockKey> chain;
  chain.reserve(budget);
  std::lock_guard<std::mutex> lock(mu_);
  BlockKey cursor = head;
  for (std::size_t i = 0; i < budget; ++i) {
    auto it = successor_.find(cursor);
    if (it == successor_.end()) {
      break;
    }
    cursor = it->second;
    if (cursor == head) {
      break;  // 环保护：后继链绕回锚点时终止
    }
    chain.push_back(cursor);
  }
  return chain;
}

std::vector<BlockKey> PrefixPrefetcher::PredictChainMulti(
    const std::vector<BlockKey>& anchors, std::size_t budget) const {
  std::vector<BlockKey> merged;
  std::unordered_set<BlockKey> seen;
  std::lock_guard<std::mutex> lock(mu_);
  for (const BlockKey& anchor : anchors) {
    BlockKey cursor = anchor;
    for (std::size_t i = 0; i < budget; ++i) {
      auto it = successor_.find(cursor);
      if (it == successor_.end()) {
        break;
      }
      cursor = it->second;
      if (cursor == anchor) {
        break;  // 环保护：后继链绕回锚点时终止
      }
      if (seen.insert(cursor).second) {
        merged.push_back(cursor);
      }
    }
  }
  return merged;
}

std::size_t PrefixPrefetcher::EdgeCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return successor_.size();
}

void PrefixPrefetcher::InsertEdgeLocked(const BlockKey& from,
                                        const BlockKey& to) {
  auto [it, inserted] = successor_.try_emplace(from, to);
  if (!inserted) {
    it->second = to;  // 后写覆盖：以最新观察为准
    return;
  }
  fifo_.push_back(from);
  while (successor_.size() > max_edges_ && !fifo_.empty()) {
    successor_.erase(fifo_.front());
    fifo_.pop_front();
  }
}

}  // namespace project::storage
