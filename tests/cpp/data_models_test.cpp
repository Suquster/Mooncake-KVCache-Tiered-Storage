// =============================================================================
// tests/cpp/data_models_test.cpp —— 任务 3.1 核心数据模型与适配接口单元测试
// =============================================================================
// 验证项目自有数据模型（BlockKey/KVCacheBlock/SerializedBlock/NodeId/DType）的
// 结构相等语义、Result<T>/Status 错误模型、以及适配层接口头文件（ITransferBackend/
// IObjectStore 及其数据类型）可被包含、实例化、链接。不依赖任何第三方测试框架，
// 保证受限环境中始终可构建运行（CTest）。
//
// 说明：本测试聚焦「数据模型 + 接口契约」（任务 3.1）；针对适配器具体实现的端到端
// 行为测试（fake backend 的 register/put/get/locate）由任务 3.4 覆盖。此处用一个最小
// 内存 fake 仅证明接口可被实现/调用，确保接口签名自洽且可链接。
// =============================================================================
#include <cstdio>
#include <future>
#include <vector>

#include "adapter/store_adapter.h"
#include "adapter/transfer_engine_adapter.h"
#include "project/status.h"
#include "project/types.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "[失败] %s\n", what);
    ++g_failures;
  }
}

using project::BlockKey;
using project::DType;
using project::KVCacheBlock;
using project::NodeId;
using project::Result;
using project::SerializedBlock;
using project::Status;
using project::StatusCode;
namespace adapter = project::adapter;

// 构造一个用于相等性测试的样例块。
KVCacheBlock MakeBlock(std::uint64_t hash_id) {
  KVCacheBlock b;
  b.key = BlockKey{hash_id, /*layer=*/3, /*version=*/1};
  b.num_tokens = 16;
  b.num_heads = 8;
  b.head_dim = 64;
  b.dtype = DType::kBFloat16;
  b.k_payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  b.v_payload = {std::byte{0xAA}, std::byte{0xBB}};
  return b;
}

// 一个最小内存 fake，仅证明 IObjectStore 接口可被实现并调用（任务 3.4 做完整测试）。
class FakeObjectStore final : public adapter::IObjectStore {
 public:
  Status Put(const BlockKey&, const SerializedBlock&) override {
    return Status::Ok();
  }
  Result<SerializedBlock> Get(const BlockKey&) override {
    return Status{StatusCode::kNotFound, "fake：未存任何对象"};
  }
  bool Exists(const BlockKey&) const override { return false; }
  std::vector<NodeId> Locate(const BlockKey&) const override { return {}; }
};

void TestBlockKeyEquality() {
  BlockKey a{0xDEADBEEF, 7, 1};
  BlockKey b{0xDEADBEEF, 7, 1};
  BlockKey c{0xDEADBEEF, 8, 1};
  Check(a == b, "相同字段的 BlockKey 应结构相等");
  Check(!(a == c), "layer 不同的 BlockKey 不应相等");
  Check(a < c, "BlockKey 全序：layer 较小者应排在前");
}

void TestKVCacheBlockEquality() {
  KVCacheBlock b1 = MakeBlock(0x1111);
  KVCacheBlock b2 = MakeBlock(0x1111);
  KVCacheBlock b3 = MakeBlock(0x2222);
  Check(b1 == b2, "字段与 payload 全等的 KVCacheBlock 应结构相等");
  Check(!(b1 == b3), "hash_id 不同的 KVCacheBlock 不应相等");

  KVCacheBlock b4 = MakeBlock(0x1111);
  b4.v_payload.push_back(std::byte{0xCC});
  Check(!(b1 == b4), "payload 字节不同的 KVCacheBlock 不应相等");
}

void TestSerializedBlockEquality() {
  SerializedBlock s1{{std::byte{0x10}, std::byte{0x20}}};
  SerializedBlock s2{{std::byte{0x10}, std::byte{0x20}}};
  SerializedBlock s3{{std::byte{0x10}, std::byte{0x21}}};
  Check(s1 == s2, "相同字节的 SerializedBlock 应结构相等");
  Check(!(s1 == s3), "字节不同的 SerializedBlock 不应相等");
}

void TestDType() {
  Check(project::DTypeSizeBytes(DType::kFloat32) == 4, "fp32 应为 4 字节");
  Check(project::DTypeSizeBytes(DType::kFloat16) == 2, "fp16 应为 2 字节");
  Check(project::DTypeSizeBytes(DType::kInt8) == 1, "int8 应为 1 字节");
  Check(std::string(project::DTypeName(DType::kBFloat16)) == "bf16",
        "bf16 短名应为 \"bf16\"");
}

void TestStatusAndResult() {
  Status ok = Status::Ok();
  Check(ok.ok() && ok.code() == StatusCode::kOk, "默认/Ok 状态应为 kOk");

  Status err{StatusCode::kNotFound, "缺失"};
  Check(!err.ok() && err.code() == StatusCode::kNotFound, "错误状态码应为 kNotFound");
  Check(err.ToString() == "kNotFound: 缺失", "ToString 应拼装码名与消息");

  Result<int> good = 42;
  Check(good.ok() && good.value() == 42, "成功 Result 应持有值 42");

  Result<int> bad = Status{StatusCode::kInvalidArgument, "坏"};
  Check(!bad.ok() && bad.status().code() == StatusCode::kInvalidArgument,
        "失败 Result 应携带错误状态");
  Check(bad.value_or(-1) == -1, "失败 Result 的 value_or 应返回回退值");
}

void TestNodeId() {
  NodeId n1{"10.0.0.1:50051"};
  NodeId n2{"10.0.0.1:50051"};
  NodeId n3{"10.0.0.2:50051"};
  Check(n1 == n2, "相同字符串的 NodeId 应相等");
  Check(n1 < n3, "NodeId 应支持全序比较");
}

void TestAdapterInterfaceTypes() {
  // TransportPath 短名（adapter_types.cpp 实现，验证可链接）。
  Check(std::string(adapter::TransportPathName(adapter::TransportPath::kRdma)) ==
            "rdma",
        "kRdma 短名应为 \"rdma\"");
  Check(std::string(adapter::TransportPathName(
            adapter::TransportPath::kGpuDirect)) == "gpudirect",
        "kGpuDirect 短名应为 \"gpudirect\"");
  Check(std::string(adapter::TransportPathName(adapter::TransportPath::kTcp)) ==
            "tcp",
        "kTcp 短名应为 \"tcp\"");

  // TransferReceipt / TransferError / BufferHandle / TransferRequest 的结构相等。
  adapter::TransferReceipt r1{BlockKey{1, 0, 1}, adapter::TransportPath::kRdma,
                              128};
  adapter::TransferReceipt r2{BlockKey{1, 0, 1}, adapter::TransportPath::kRdma,
                              128};
  Check(r1 == r2, "相同字段的 TransferReceipt 应结构相等");

  adapter::TransferError e1{BlockKey{2, 0, 1}, adapter::TransportPath::kTcp,
                            "超时"};
  adapter::TransferError e2{BlockKey{2, 0, 1}, adapter::TransportPath::kTcp,
                            "超时"};
  Check(e1 == e2, "相同字段的 TransferError 应结构相等");
  Check((e1.failing_key == BlockKey{2, 0, 1}),
        "TransferError 应携带失败块键（需求 3.6）");

  // IObjectStore 可被实现并调用（接口契约自洽 + 可链接）。
  FakeObjectStore store;
  Check(store.Put(BlockKey{3, 0, 1}, SerializedBlock{}).ok(),
        "fake Put 应返回 Ok");
  Check(!store.Get(BlockKey{3, 0, 1}).ok(),
        "fake Get 未命中应返回非 Ok 的 Result");
  Check(!store.Exists(BlockKey{3, 0, 1}), "fake Exists 应返回 false");
  Check(store.Locate(BlockKey{3, 0, 1}).empty(), "fake Locate 应返回空集合");
}

}  // namespace

int main() {
  TestBlockKeyEquality();
  TestKVCacheBlockEquality();
  TestSerializedBlockEquality();
  TestDType();
  TestStatusAndResult();
  TestNodeId();
  TestAdapterInterfaceTypes();

  if (g_failures == 0) {
    std::printf("[通过] 数据模型与适配接口单元测试全部通过\n");
  }
  return g_failures == 0 ? 0 : 1;
}
