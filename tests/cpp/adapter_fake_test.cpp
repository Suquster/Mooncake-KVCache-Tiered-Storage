// 适配层接线单元测试（任务 3.4）：以内存态替身验证边界契约——
// register/deregister 配对、put/get/exists/locate、路径选择与失败注入。
// 无外部依赖（不需 RapidCheck / 硬件 / 上游服务）。
#include <cassert>
#include <cstdio>
#include <vector>

#include "adapter/fake_backend.h"
#include "datapath/data_path.h"

using project::BlockKey;
using project::KVCacheBlock;
using project::NodeId;
using project::SerializedBlock;
using project::StatusCode;
using project::adapter::BufferHandle;
using project::adapter::FakeTransferBackend;
using project::adapter::InMemoryObjectStore;
using project::adapter::TransferRequest;
using project::adapter::TransportPath;

namespace {

int g_failures = 0;

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

void TestRegisterDeregisterPairing() {
  FakeTransferBackend backend;
  std::vector<std::byte> buf(64);
  auto reg = backend.RegisterBuffer(buf.data(), buf.size(), /*on_gpu=*/false);
  EXPECT(reg.ok());
  EXPECT(backend.live_registration_count() == 1);
  EXPECT(backend.DeregisterBuffer(reg.value()).ok());
  EXPECT(backend.live_registration_count() == 0);
  // 重复注销必须报错（配对约束，需求 7.3）。
  EXPECT(backend.DeregisterBuffer(reg.value()).code() ==
         StatusCode::kAddressNotRegistered);
  // 空缓冲不可注册。
  EXPECT(!backend.RegisterBuffer(nullptr, 0, false).ok());
}

void TestPathSelectionAndFailureInjection() {
  FakeTransferBackend backend;
  std::vector<std::byte> src(16, std::byte{7});
  std::vector<std::byte> dst(16);
  auto src_reg = backend.RegisterBuffer(src.data(), src.size(), false);
  auto dst_reg = backend.RegisterBuffer(dst.data(), dst.size(), false);
  TransferRequest req{BlockKey{42, 3}, src_reg.value(), dst_reg.value(),
                      TransportPath::kRdma};

  // RDMA 可用 → 选 RDMA；不可用 → 回退 TCP（需求 3.1 / 3.7）。
  EXPECT(backend.SelectPath(req) == TransportPath::kRdma);
  backend.rdma_available = false;
  EXPECT(backend.SelectPath(req) == TransportPath::kTcp);
  backend.rdma_available = true;

  // 成功传输：零拷贝搬运到目标缓冲。
  auto receipt = backend.SubmitAsync(req).get();
  EXPECT(receipt.ok());
  EXPECT(receipt.value().transferred_bytes == 16);
  EXPECT(dst == src);

  // 失败注入：错误携带失败块键（需求 3.6）。
  backend.fail_next_transfer = 1;
  auto failed = backend.SubmitAsync(req).get();
  EXPECT(!failed.ok());
  EXPECT(failed.status().code() == StatusCode::kTransferFailed);
  EXPECT(backend.last_error().failing_key == req.key);

  EXPECT(backend.DeregisterBuffer(src_reg.value()).ok());
  EXPECT(backend.DeregisterBuffer(dst_reg.value()).ok());
}

void TestObjectStoreContract() {
  InMemoryObjectStore store(NodeId{"10.0.0.1:8080"});
  const BlockKey key{7, 1};
  SerializedBlock block;
  block.bytes = {std::byte{1}, std::byte{2}, std::byte{3}};

  EXPECT(!store.Exists(key));
  EXPECT(store.Get(key).status().code() == StatusCode::kNotFound);
  EXPECT(store.Locate(key).empty());

  EXPECT(store.Put(key, block).ok());
  EXPECT(store.Exists(key));
  auto got = store.Get(key);
  EXPECT(got.ok());
  EXPECT(got.value() == block);
  const auto holders = store.Locate(key);
  EXPECT(holders.size() == 1 && holders[0] == NodeId{"10.0.0.1:8080"});
}

void TestScopedRegistrationRaii() {
  FakeTransferBackend backend;
  std::vector<std::byte> buf(32);
  {
    project::datapath::ScopedRegistration guard(backend, buf.data(),
                                                buf.size(), false);
    EXPECT(guard.valid());
    EXPECT(backend.live_registration_count() == 1);
  }
  // 作用域退出即注销（需求 7.3）。
  EXPECT(backend.live_registration_count() == 0);
  EXPECT(backend.register_count() == backend.deregister_count());
}

}  // namespace

int main() {
  TestRegisterDeregisterPairing();
  TestPathSelectionAndFailureInjection();
  TestObjectStoreContract();
  TestScopedRegistrationRaii();
  if (g_failures != 0) {
    std::fprintf(stderr, "adapter_fake_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("adapter_fake_test: all assertions passed\n");
  return 0;
}
