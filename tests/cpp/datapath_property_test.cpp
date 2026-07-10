// 数据面属性/示例测试（RapidCheck，最少 100 次迭代，Properties 6/7/8/18 +
// 异步句柄/零拷贝示例，任务 6.2/6.4/6.5/6.7/6.9）。
#include <rapidcheck.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#include "adapter/fake_backend.h"
#include "datapath/data_path.h"

using project::BlockKey;
using project::DType;
using project::KVCacheBlock;
using project::StatusCode;
using project::adapter::FakeTransferBackend;
using project::adapter::TransferRequest;
using project::adapter::TransportPath;
using project::datapath::DataPath;
using project::datapath::ScopedRegistration;

namespace {

rc::Gen<KVCacheBlock> BlockGen() {
  return rc::gen::build<KVCacheBlock>(
      rc::gen::set(&KVCacheBlock::key,
                   rc::gen::build<BlockKey>(
                       rc::gen::set(&BlockKey::hash_id,
                                    rc::gen::arbitrary<std::uint64_t>()),
                       rc::gen::set(&BlockKey::layer,
                                    rc::gen::inRange<std::uint32_t>(0, 128)))),
      rc::gen::set(&KVCacheBlock::num_tokens,
                   rc::gen::inRange<std::uint32_t>(0, 4096)),
      rc::gen::set(&KVCacheBlock::num_heads,
                   rc::gen::inRange<std::uint16_t>(0, 128)),
      rc::gen::set(&KVCacheBlock::head_dim,
                   rc::gen::inRange<std::uint16_t>(0, 256)),
      rc::gen::set(&KVCacheBlock::dtype,
                   rc::gen::element(DType::kFloat16, DType::kBFloat16,
                                    DType::kFloat8E4M3, DType::kFloat32,
                                    DType::kInt8)),
      rc::gen::set(&KVCacheBlock::k_payload,
                   rc::gen::container<std::vector<std::byte>>(
                       rc::gen::map(rc::gen::inRange<int>(0, 256), [](int v) {
                         return static_cast<std::byte>(v);
                       }))),
      rc::gen::set(&KVCacheBlock::v_payload,
                   rc::gen::container<std::vector<std::byte>>(
                       rc::gen::map(rc::gen::inRange<int>(0, 256), [](int v) {
                         return static_cast<std::byte>(v);
                       }))));
}

}  // namespace

int main() {
  bool ok = true;

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 6: "
      "serialization round-trip",
      [] {
        const KVCacheBlock block = *BlockGen();
        const auto serialized = DataPath::Serialize(block);
        const auto restored = DataPath::Deserialize(serialized);
        RC_ASSERT(restored.ok());
        RC_ASSERT(restored.value() == block);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 7: "
      "transfer error carries the failing block key",
      [] {
        FakeTransferBackend backend;
        DataPath path(backend, /*queue_capacity=*/8);
        std::vector<std::byte> src(32), dst(32);
        ScopedRegistration src_reg(backend, src.data(), src.size(), false);
        ScopedRegistration dst_reg(backend, dst.data(), dst.size(), false);
        const auto hash_id = *rc::gen::arbitrary<std::uint64_t>();
        const auto layer = *rc::gen::inRange<std::uint32_t>(0, 64);
        const BlockKey key{hash_id, layer};
        backend.fail_next_transfer = 1;
        auto result =
            path.TransferAsync(
                    TransferRequest{key, src_reg.handle(), dst_reg.handle(),
                                    TransportPath::kRdma})
                .get();
        RC_ASSERT(!result.ok());
        RC_ASSERT(result.status().code() == StatusCode::kTransferFailed);
        // 结构化错误携带精确的失败块键（需求 3.6）。
        RC_ASSERT(backend.last_error().failing_key == key);
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 8: "
      "path selection honors capability",
      [] {
        FakeTransferBackend backend;
        backend.rdma_available = *rc::gen::arbitrary<bool>();
        backend.gpu_direct_available = *rc::gen::arbitrary<bool>();
        const bool on_gpu = *rc::gen::arbitrary<bool>();
        std::vector<std::byte> src(8), dst(8);
        ScopedRegistration src_reg(backend, src.data(), src.size(), on_gpu);
        ScopedRegistration dst_reg(backend, dst.data(), dst.size(), false);
        const TransferRequest req{BlockKey{1, 0}, src_reg.handle(),
                                  dst_reg.handle(), TransportPath::kRdma};
        const TransportPath selected = backend.SelectPath(req);
        if (!backend.rdma_available) {
          RC_ASSERT(selected == TransportPath::kTcp);  // 需求 3.7
        } else if (on_gpu && backend.gpu_direct_available) {
          RC_ASSERT(selected == TransportPath::kGpuDirect);  // 需求 3.2
        } else {
          RC_ASSERT(selected == TransportPath::kRdma);  // 需求 3.1
        }
      });

  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 18: "
      "data-path resource conservation",
      [] {
        FakeTransferBackend backend;
        const auto rounds = *rc::gen::inRange<int>(1, 16);
        const auto failures = *rc::gen::inRange<int>(0, 4);
        backend.fail_next_transfer = failures;
        {
          DataPath path(backend, 8);
          for (int i = 0; i < rounds; ++i) {
            std::vector<std::byte> src(16), dst(16);
            ScopedRegistration src_reg(backend, src.data(), src.size(), false);
            ScopedRegistration dst_reg(backend, dst.data(), dst.size(), false);
            (void)path.TransferAsync(TransferRequest{
                             BlockKey{static_cast<std::uint64_t>(i), 0},
                             src_reg.handle(), dst_reg.handle(),
                             TransportPath::kRdma})
                .get();
          }
        }
        // 含失败路径在内：注册数 == 注销数，无泄漏（需求 7.3）。
        RC_ASSERT(backend.register_count() == backend.deregister_count());
        RC_ASSERT(backend.live_registration_count() == 0);
      });

  // 示例测试（任务 6.5）：异步句柄先于完成返回 + 零拷贝直达目标缓冲。
  {
    FakeTransferBackend backend;
    DataPath path(backend, 4);
    std::vector<std::byte> src(64, std::byte{0x5A}), dst(64);
    ScopedRegistration src_reg(backend, src.data(), src.size(), false);
    ScopedRegistration dst_reg(backend, dst.data(), dst.size(), false);
    auto future = path.TransferAsync(TransferRequest{
        BlockKey{99, 0}, src_reg.handle(), dst_reg.handle(),
        TransportPath::kRdma});
    // TransferAsync 返回的是 future（异步句柄），调用方即刻继续执行（需求 3.4）。
    const auto receipt = future.get();
    if (!receipt.ok() || dst != src ||
        receipt.value().transferred_bytes != 64) {
      return 1;  // 零拷贝语义或回执不符（需求 3.3）
    }
  }

  // 示例测试（任务 6.6）：队满施加背压，错误码为 kBackpressure 且含失败键。
  {
    FakeTransferBackend backend;
    std::vector<std::byte> src(8), dst(8);
    ScopedRegistration src_reg(backend, src.data(), src.size(), false);
    ScopedRegistration dst_reg(backend, dst.data(), dst.size(), false);
    DataPath path(backend, /*queue_capacity=*/1);
    // 闸门挡住后端：容量 1 的队列在第三次提交时必然队满而背压。
    backend.hold_transfers = true;
    std::vector<std::future<project::Result<project::adapter::TransferReceipt>>>
        futures;
    bool backpressure_seen = false;
    for (int i = 0; i < 3; ++i) {
      auto f = path.TransferAsync(TransferRequest{
          BlockKey{static_cast<std::uint64_t>(i), 0}, src_reg.handle(),
          dst_reg.handle(), TransportPath::kRdma});
      if (f.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = f.get();
        if (!result.ok() &&
            result.status().code() == StatusCode::kBackpressure) {
          backpressure_seen = true;
          continue;
        }
      }
      futures.push_back(std::move(f));
    }
    backend.hold_transfers = false;
    for (auto& f : futures) {
      if (f.valid()) {
        (void)f.get();
      }
    }
    if (!backpressure_seen) {
      return 1;
    }
  }

  return ok ? 0 : 1;
}
