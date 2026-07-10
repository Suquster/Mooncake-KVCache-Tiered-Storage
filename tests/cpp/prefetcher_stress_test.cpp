// =============================================================================
// tests/cpp/prefetcher_stress_test.cpp —— 前缀预取器并发压测（TSan 覆盖）
// =============================================================================
// 多线程交叉执行 RecordSequence / PredictChain / PredictChainMulti /
// EdgeCount，验证互斥量守护下无数据竞争（需求 7.2）；三套消毒器矩阵中的
// TSan 构建对本测试做数据竞争检测。压测后单线程校验后继图仍满足
// 「后写覆盖 + 定容」不变式。
// =============================================================================
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "project/types.h"
#include "storage/prefix_prefetcher.h"

using project::BlockKey;
using project::storage::PrefixPrefetcher;

namespace {

constexpr int kThreads = 8;
constexpr int kIterations = 2000;
constexpr std::size_t kEdgeCap = 512;

BlockKey Key(std::uint64_t hash_id) { return BlockKey{hash_id, 0}; }

}  // namespace

int main() {
  PrefixPrefetcher prefetcher(kEdgeCap);
  std::atomic<bool> failed{false};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &prefetcher, &failed] {
      for (int i = 0; i < kIterations; ++i) {
        // 每线程写入自己的键区间链（互相交叠触发后写覆盖路径）。
        const std::uint64_t base =
            static_cast<std::uint64_t>((t % 4) * 64 + i % 32);
        prefetcher.RecordSequence(
            {Key(base), Key(base + 1), Key(base + 2), Key(base + 3)});
        const auto chain = prefetcher.PredictChain(Key(base), 8);
        if (chain.size() > 8) {
          failed.store(true);
        }
        const auto multi = prefetcher.PredictChainMulti(
            {Key(base), Key(base + 1), Key(base + 2)}, 8);
        if (multi.size() > 24) {
          failed.store(true);
        }
        if (prefetcher.EdgeCount() > kEdgeCap) {
          failed.store(true);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  if (failed.load()) {
    std::puts("FAIL: 并发压测期间观测到不变式违例");
    return 1;
  }

  // 压测后单线程校验：链预测仍按后继图顺序给出且有界。
  prefetcher.RecordSequence({Key(9001), Key(9002), Key(9003)});
  const auto chain = prefetcher.PredictChain(Key(9001), 8);
  if (chain.size() != 2 || !(chain[0] == Key(9002)) ||
      !(chain[1] == Key(9003))) {
    std::puts("FAIL: 压测后的后继链预测不符合预期");
    return 1;
  }
  if (prefetcher.EdgeCount() > kEdgeCap) {
    std::puts("FAIL: 后继图超出定容上限");
    return 1;
  }
  std::puts("OK: prefetcher_stress_test 全部通过");
  return 0;
}
