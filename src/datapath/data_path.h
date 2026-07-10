// =============================================================================
// src/datapath/data_path.h —— 数据面：序列化、零拷贝异步传输流水线
// =============================================================================
// 设计依据：design.md「Data_Path」。
//   * Serialize/Deserialize：定长前缀、带版本的 [header | shape/dtype | K | V]
//     线缆形态，往返重建结构相等的 KVCacheBlock（需求 3.5，Property 6）。
//   * TransferAsync：经 ITransferBackend 的零拷贝注册缓冲提交异步传输，
//     RDMA/GPUDirect 可用时优先，否则 TCP 回退（需求 3.1/3.2/3.3/3.7），
//     调用返回 pending future（需求 3.4）。
//   * 有界提交队列：队满施加背压（kBackpressure，需求 3.4）；失败以
//     TransferError{failing_key, path, reason} 上报（需求 3.6）。
//   * ScopedRegistration：RAII 保证 Register/Deregister 严格配对（需求 7.3）。
// =============================================================================
#ifndef PROJECT_DATAPATH_DATA_PATH_H_
#define PROJECT_DATAPATH_DATA_PATH_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "adapter/transfer_engine_adapter.h"
#include "project/status.h"
#include "project/types.h"

namespace project::datapath {

// RAII 注册守卫：构造时注册缓冲，析构时注销；移动语义转移所有权（需求 7.3）。
class ScopedRegistration {
 public:
  // 注册失败时 valid()==false，错误经 status() 暴露；不抛异常。
  ScopedRegistration(adapter::ITransferBackend& backend, void* addr,
                     std::size_t len, bool on_gpu);
  ~ScopedRegistration();

  ScopedRegistration(ScopedRegistration&& other) noexcept;
  ScopedRegistration& operator=(ScopedRegistration&& other) noexcept;
  ScopedRegistration(const ScopedRegistration&) = delete;
  ScopedRegistration& operator=(const ScopedRegistration&) = delete;

  bool valid() const noexcept { return valid_; }
  const Status& status() const noexcept { return status_; }
  const adapter::BufferHandle& handle() const noexcept { return handle_; }

 private:
  void Release() noexcept;

  adapter::ITransferBackend* backend_ = nullptr;
  adapter::BufferHandle handle_;
  Status status_;
  bool valid_ = false;
};

class DataPath {
 public:
  // queue_capacity：有界提交队列容量；0 视为 1（禁止无界队列）。
  explicit DataPath(adapter::ITransferBackend& backend,
                    std::size_t queue_capacity = 64);
  ~DataPath();

  DataPath(const DataPath&) = delete;
  DataPath& operator=(const DataPath&) = delete;

  // 序列化：[u64 总长 | magic | 版本 | 键 | 形状/dtype | K | V]（需求 3.5）。
  static SerializedBlock Serialize(const KVCacheBlock& block);
  // 反序列化：校验长度前缀/魔数/版本；损坏输入返回 kInvalidArgument。
  static Result<KVCacheBlock> Deserialize(const SerializedBlock& serialized);

  // 异步传输：入队立即返回 pending future（需求 3.4）。路径由后端能力选定
  // （需求 3.1/3.2/3.7）；队满立即以 kBackpressure（含失败块键）兑现（需求 3.6）。
  std::future<Result<adapter::TransferReceipt>> TransferAsync(
      const adapter::TransferRequest& request);

  // 当前排队中的请求数（观测/测试用）。
  std::size_t PendingDepth() const;

 private:
  struct PendingItem {
    adapter::TransferRequest request;
    std::promise<Result<adapter::TransferReceipt>> promise;
  };

  void WorkerLoop();

  adapter::ITransferBackend& backend_;
  const std::size_t queue_capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<PendingItem> queue_;
  bool shutting_down_ = false;
  std::thread worker_;
};

}  // namespace project::datapath

#endif  // PROJECT_DATAPATH_DATA_PATH_H_
