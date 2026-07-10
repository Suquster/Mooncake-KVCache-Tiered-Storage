// =============================================================================
// src/adapter/mooncake_transfer_backend.h —— ITransferBackend 的上游生产实现
// =============================================================================
// 唯一允许包含上游 Mooncake 头文件的层（需求 6.2）。基于上游 Transfer Engine
// 的稳定 C ABI（transfer_engine_c.h）实现：
//   RegisterBuffer/DeregisterBuffer ← registerLocalMemory/unregisterLocalMemory
//   SubmitAsync                     ← allocateBatchID + submitTransfer + getTransferStatus
//   SelectPath                      ← 构造期探测已安装 transport（rdma/tcp）
// 仅在 -DWITH_MOONCAKE_TE=ON 且提供上游库时编译链接；接口本身不泄漏上游类型。
// =============================================================================
#ifndef PROJECT_ADAPTER_MOONCAKE_TRANSFER_BACKEND_H_
#define PROJECT_ADAPTER_MOONCAKE_TRANSFER_BACKEND_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "adapter/transfer_engine_adapter.h"
#include "project/status.h"
#include "project/types.h"

namespace project::adapter {

// 上游引擎的连接配置。
struct MooncakeBackendConfig {
  std::string metadata_conn_string;  // 如 "etcd://127.0.0.1:2379" 或 "P2PHANDSHAKE"
  std::string local_server_name;     // 本节点段名
  std::string ip_or_host_name;       // 本节点可达地址
  std::uint64_t rpc_port = 12345;
  bool try_rdma = true;              // false 时只装 TCP transport（需求 3.7）
  std::string gpu_location = "cuda:0";  // GPUDirect 缓冲的 location 标签
  std::string remote_segment_name;   // 目标段名；空 ⇒ 本地段（单节点接线）
};

class MooncakeTransferBackend final : public ITransferBackend {
 public:
  // 工厂：创建并初始化上游引擎；失败经 Status 报告（不跨边界抛异常）。
  static Result<std::unique_ptr<MooncakeTransferBackend>> Create(
      const MooncakeBackendConfig& config);

  ~MooncakeTransferBackend() override;

  MooncakeTransferBackend(const MooncakeTransferBackend&) = delete;
  MooncakeTransferBackend& operator=(const MooncakeTransferBackend&) = delete;

  Result<BufferHandle> RegisterBuffer(void* addr, std::size_t len,
                                      bool on_gpu) override;
  Status DeregisterBuffer(const BufferHandle& handle) override;
  TransportPath SelectPath(const TransferRequest& request) const override;
  std::future<Result<TransferReceipt>> SubmitAsync(
      TransferRequest request) override;

 private:
  MooncakeTransferBackend(void* engine, MooncakeBackendConfig config,
                          bool rdma_installed, std::int32_t segment);

  void* engine_;  // 上游 transfer_engine_t（不透明指针，析构时销毁）
  MooncakeBackendConfig config_;
  bool rdma_installed_;
  std::int32_t segment_;  // openSegment 解析的目标段句柄
  mutable std::mutex mu_;
  std::uint64_t next_reg_id_ = 1;
  std::unordered_map<std::uint64_t, void*> live_registrations_;  // reg_id → addr
};

}  // namespace project::adapter

#endif  // PROJECT_ADAPTER_MOONCAKE_TRANSFER_BACKEND_H_
