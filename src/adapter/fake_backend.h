// =============================================================================
// src/adapter/fake_backend.h —— 适配层接口的内存态测试替身（任务 3.4）
// =============================================================================
// 目的：在无 RDMA/GPU 硬件与上游服务的环境中，让适配器边界（ITransferBackend /
// IObjectStore）可被完整测试。真实适配器（transfer_engine_adapter.cpp /
// store_adapter.cpp）与本替身实现同一接口，可互换注入。
//
// 能力：
//   * 能力开关（rdma_available / gpu_direct_available）驱动路径选择测试
//     （Property 8，需求 3.1/3.2/3.7）。
//   * 失败注入（fail_next_transfer）驱动失败键上报测试（Property 7，需求 3.6）。
//   * 注册/注销计数器驱动资源守恒测试（Property 18，需求 7.3）。
//   * 零拷贝语义：传输直接在已注册的 src/dst 缓冲间 memcpy，不经过中转缓冲
//     （需求 3.3 的可观察等价物）。
// =============================================================================
#ifndef PROJECT_ADAPTER_FAKE_BACKEND_H_
#define PROJECT_ADAPTER_FAKE_BACKEND_H_

#include <atomic>
#include <cstdint>
#include <thread>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adapter/store_adapter.h"
#include "adapter/transfer_engine_adapter.h"
#include "project/status.h"
#include "project/types.h"

namespace project::adapter {

// ITransferBackend 的内存态替身。
class FakeTransferBackend : public ITransferBackend {
 public:
  // 能力开关（默认全开）。
  bool rdma_available = true;
  bool gpu_direct_available = true;
  // 失败注入：>0 时接下来 N 次 SubmitAsync 失败（携带失败块键）。
  std::atomic<int> fail_next_transfer{0};
  // 传输闸门：true 时 SubmitAsync 自旋等待，供背压测试确定性地填满队列。
  std::atomic<bool> hold_transfers{false};

  Result<BufferHandle> RegisterBuffer(void* addr, std::size_t len,
                                      bool on_gpu) override {
    if (addr == nullptr || len == 0) {
      return Result<BufferHandle>(
          Status::Make(StatusCode::kInvalidArgument, "空缓冲不可注册"));
    }
    std::lock_guard<std::mutex> lock(mu_);
    BufferHandle handle{addr, len, next_reg_id_++, on_gpu};
    live_registrations_.insert(handle.reg_id);
    register_count_ += 1;
    return Result<BufferHandle>(handle);
  }

  Status DeregisterBuffer(const BufferHandle& handle) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (live_registrations_.erase(handle.reg_id) == 0) {
      return Status::Make(StatusCode::kAddressNotRegistered,
                          "句柄未注册或已注销");
    }
    deregister_count_ += 1;
    return Status::Ok();
  }

  TransportPath SelectPath(const TransferRequest& request) const override {
    // 能力优先序：GPUDirect（显存参与且可用）> RDMA > TCP（需求 3.1/3.2/3.7）。
    const bool touches_gpu = request.src.on_gpu || request.dst.on_gpu;
    if (touches_gpu && gpu_direct_available && rdma_available) {
      return TransportPath::kGpuDirect;
    }
    if (rdma_available) {
      return TransportPath::kRdma;
    }
    return TransportPath::kTcp;
  }

  std::future<Result<TransferReceipt>> SubmitAsync(
      TransferRequest request) override {
    while (hold_transfers.load()) {
      std::this_thread::yield();
    }
    const TransportPath path = SelectPath(request);
    std::promise<Result<TransferReceipt>> promise;
    auto future = promise.get_future();
    {
      std::lock_guard<std::mutex> lock(mu_);
      submit_count_ += 1;
      if (live_registrations_.count(request.src.reg_id) == 0 ||
          live_registrations_.count(request.dst.reg_id) == 0) {
        promise.set_value(Result<TransferReceipt>(Status::Make(
            StatusCode::kAddressNotRegistered,
            "源/目标缓冲未注册，零拷贝前提不满足；failing_key.hash_id=" +
                std::to_string(request.key.hash_id))));
        return future;
      }
    }
    if (fail_next_transfer.load() > 0) {
      fail_next_transfer.fetch_sub(1);
      last_error_ = TransferError{
          request.key, path,
          "注入的传输失败（测试）；failing_key.hash_id=" +
              std::to_string(request.key.hash_id)};
      promise.set_value(Result<TransferReceipt>(
          Status::Make(StatusCode::kTransferFailed, last_error_.reason)));
      return future;
    }
    // 零拷贝等价物：直接在已注册缓冲间搬运，不引入任何中转缓冲。
    const std::size_t bytes =
        request.src.length < request.dst.length ? request.src.length
                                                : request.dst.length;
    std::memcpy(request.dst.addr, request.src.addr, bytes);
    promise.set_value(
        Result<TransferReceipt>(TransferReceipt{request.key, path, bytes}));
    return future;
  }

  // 观测接口（测试断言用）。
  std::uint64_t register_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return register_count_;
  }
  std::uint64_t deregister_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return deregister_count_;
  }
  std::size_t live_registration_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return live_registrations_.size();
  }
  std::uint64_t submit_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return submit_count_;
  }
  const TransferError& last_error() const { return last_error_; }

 private:
  mutable std::mutex mu_;
  std::uint64_t next_reg_id_ = 1;
  std::unordered_set<std::uint64_t> live_registrations_;
  std::uint64_t register_count_ = 0;
  std::uint64_t deregister_count_ = 0;
  std::uint64_t submit_count_ = 0;
  TransferError last_error_;
};

// IObjectStore 的内存态替身：单节点持有全部对象，Locate 返回本节点。
class InMemoryObjectStore : public IObjectStore {
 public:
  explicit InMemoryObjectStore(NodeId self = NodeId{"127.0.0.1:0"})
      : self_(std::move(self)) {}

  Status Put(const BlockKey& key, const SerializedBlock& block) override {
    std::lock_guard<std::mutex> lock(mu_);
    objects_[key] = block;
    return Status::Ok();
  }

  Result<SerializedBlock> Get(const BlockKey& key) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(key);
    if (it == objects_.end()) {
      return Result<SerializedBlock>(
          Status::Make(StatusCode::kNotFound, "对象不存在（缓存未命中）"));
    }
    return Result<SerializedBlock>(it->second);
  }

  bool Exists(const BlockKey& key) const override {
    std::lock_guard<std::mutex> lock(mu_);
    return objects_.count(key) > 0;
  }

  std::vector<NodeId> Locate(const BlockKey& key) const override {
    std::lock_guard<std::mutex> lock(mu_);
    if (objects_.count(key) == 0) {
      return {};
    }
    return {self_};
  }

 private:
  NodeId self_;
  mutable std::mutex mu_;
  std::unordered_map<BlockKey, SerializedBlock> objects_;
};

}  // namespace project::adapter

#endif  // PROJECT_ADAPTER_FAKE_BACKEND_H_
