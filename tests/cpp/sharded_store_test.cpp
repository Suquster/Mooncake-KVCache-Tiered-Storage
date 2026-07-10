// 分片并发存储：正确性（属性等价 + 多线程一致性）与吞吐微基准。
// 正确性部分随 ctest 运行；微基准仅在 --bench 参数下运行（避免拖慢 CI）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "storage/sharded_tiered_store.h"

using project::BlockKey;
using project::DType;
using project::KVCacheBlock;
using project::storage::ShardedTieredStore;
using project::storage::Tier;
using project::storage::TierConfig;
using project::storage::TieredStorageManager;

namespace {

int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

KVCacheBlock MakeBlock(std::uint64_t hash_id, std::size_t payload = 64) {
  KVCacheBlock block;
  block.key = BlockKey{hash_id, 0};
  block.dtype = DType::kFloat16;
  block.k_payload.assign(payload / 2, std::byte{0xAB});
  block.v_payload.assign(payload - payload / 2, std::byte{0xCD});
  return block;
}

TierConfig BigConfig() {
  TierConfig config;
  config.hbm_capacity_bytes = 64ULL << 20;
  config.dram_capacity_bytes = 256ULL << 20;
  config.high_water_ratio = 1.0;
  return config;
}

// 正确性：写读往返、占用上界、多线程无撕裂。
void TestCorrectness() {
  ShardedTieredStore store(BigConfig(), 16);
  for (std::uint64_t i = 0; i < 1000; ++i) {
    EXPECT(store.Write(MakeBlock(i), Tier::kHBM).ok());
  }
  for (std::uint64_t i = 0; i < 1000; ++i) {
    auto got = store.Read(BlockKey{i, 0});
    EXPECT(got.ok());
    EXPECT(got.value() == MakeBlock(i));
  }
  const auto occupancy = store.Occupancy(Tier::kHBM);
  EXPECT(occupancy.used_bytes <= occupancy.capacity_bytes);

  // 多线程：不相交键空间并发写读，终态完整。
  constexpr int kThreads = 8;
  constexpr std::uint64_t kPerThread = 500;
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&store, &errors, t] {
      const std::uint64_t base = 10000 + t * kPerThread;
      for (std::uint64_t i = 0; i < kPerThread; ++i) {
        if (!store.Write(MakeBlock(base + i), Tier::kHBM).ok()) {
          errors.fetch_add(1);
        }
        if (!store.Read(BlockKey{base + i, 0}).ok()) {
          errors.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT(errors.load() == 0);
}

// 微基准：单锁 vs 16 分片，多线程混合读写吞吐。
template <typename Store>
double RunBench(Store& store, int threads_n, int ops_per_thread) {
  std::vector<std::thread> threads;
  const auto start = std::chrono::steady_clock::now();
  for (int t = 0; t < threads_n; ++t) {
    threads.emplace_back([&store, t, ops_per_thread] {
      for (int i = 0; i < ops_per_thread; ++i) {
        const std::uint64_t key =
            static_cast<std::uint64_t>(t) * ops_per_thread + i;
        (void)store.Write(MakeBlock(key), Tier::kHBM);
        (void)store.Read(BlockKey{key, 0});
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  return threads_n * ops_per_thread * 2.0 / elapsed;
}

void RunMicrobench() {
  const int threads_n = static_cast<int>(std::thread::hardware_concurrency());
  const int ops = 20000;
  TieredStorageManager single(BigConfig());
  ShardedTieredStore sharded(BigConfig(), 16);
  const double single_ops = RunBench(single, threads_n, ops);
  const double sharded_ops = RunBench(sharded, threads_n, ops);
  std::printf("threads=%d single-lock=%.0f ops/s sharded16=%.0f ops/s speedup=%.2fx\n",
              threads_n, single_ops, sharded_ops, sharded_ops / single_ops);
}

}  // namespace

int main(int argc, char** argv) {
  TestCorrectness();
  if (argc > 1 && std::string(argv[1]) == "--bench") {
    RunMicrobench();
  }
  if (g_failures != 0) {
    std::fprintf(stderr, "sharded_store_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("sharded_store_test: all assertions passed\n");
  return 0;
}
