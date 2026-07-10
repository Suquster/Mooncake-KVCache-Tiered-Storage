// =============================================================================
// src/adapter/transfer_engine_adapter.h —— Project↔Mooncake 传输适配接口
// =============================================================================
// 设计依据：design.md「Project↔Mooncake Adapter Interfaces」，并已与
// docs/analysis/upstream-mooncake-analysis.md §11.1 对账锁定（GATE: PASS）。
//
// 本头文件**仅定义项目自有抽象**（ITransferBackend 及其数据类型），不包含任何上游
// Mooncake 头文件——这是「适配器边界」的关键：仅具体实现（任务 3.2 的
// transfer_engine_adapter.cpp）才 include 上游头文件并链接其库，从而把上游 API 演进
// 的耦合面收敛到单一可审计位置（需求 6.2）。
//
// 上游支撑（分析文档 §11.1）：
//   RegisterBuffer/DeregisterBuffer  ← TransferEngine::registerLocalMemory/unregisterLocalMemory
//   SelectPath                       ← MultiTransport::selectTransport + getTransport 探测
//   SubmitAsync                      ← allocateBatchID + submitTransfer（异步返回）+ getTransferStatus
// =============================================================================
#ifndef PROJECT_ADAPTER_TRANSFER_ENGINE_ADAPTER_H_
#define PROJECT_ADAPTER_TRANSFER_ENGINE_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <future>
#include <string>

#include "project/status.h"
#include "project/types.h"

namespace project::adapter {

// 传输路径（项目侧的能力分类）。映射到上游协议名：
//   kRdma     → "rdma"（本地 DRAM/VRAM ↔ 远端 DRAM，高性能路径，需求 3.1）
//   kGpuDirect→ "rdma" + location="cuda:N"（GPU↔NIC 直通，绕过主机内存，需求 3.2）
//   kTcp      → "tcp"（仅 TCP 环境的回退路径，需求 3.7）
enum class TransportPath : std::uint8_t {
  kRdma = 0,
  kGpuDirect = 1,
  kTcp = 2,
};

// 返回传输路径的稳定短名，供日志/诊断与 TransferError.reason 拼装使用。
const char* TransportPathName(TransportPath path) noexcept;

// 不透明的零拷贝注册缓冲句柄。封装上游 registerLocalMemory 的注册结果，
// 由 RAII 守卫（ScopedRegistration，任务 6.8）保证 register/unregister 配对（需求 7.3）。
struct BufferHandle {
  void* addr = nullptr;       // 已注册缓冲首地址（调用方拥有其生命周期）
  std::size_t length = 0;     // 缓冲长度（字节）
  std::uint64_t reg_id = 0;   // 适配器内部分配的注册标识（映射上游 MR）
  bool on_gpu = false;        // true → 显存缓冲（location="cuda:N"），驱动 GPUDirect

  bool operator==(const BufferHandle& other) const = default;
};

// 单条传输请求：把某块的源缓冲搬运到目标缓冲，附带首选路径。
struct TransferRequest {
  BlockKey key;               // 关联的块键（失败时回填进 TransferError，需求 3.6）
  BufferHandle src;           // 源（已注册）缓冲
  BufferHandle dst;           // 目标（已注册）缓冲
  TransportPath preferred = TransportPath::kRdma;  // 由能力探测选定的首选路径
};

// 传输成功回执：携带块键、实际使用的路径与已搬运字节数。
struct TransferReceipt {
  BlockKey key;                       // 完成传输的块键
  TransportPath path = TransportPath::kRdma;  // 实际使用的路径
  std::size_t transferred_bytes = 0;  // 已成功搬运字节数

  bool operator==(const TransferReceipt& other) const = default;
};

// 传输失败错误：携带失败块键、所选路径与诊断原因（需求 3.6）。
// 结构化字段（非纯字符串），便于上层精确处理与失败键上报。
struct TransferError {
  BlockKey failing_key;                       // 失败的块键
  TransportPath path = TransportPath::kRdma;  // 失败时所走的路径
  std::string reason;                         // 诊断原因（含上游 Status code/message）

  bool operator==(const TransferError& other) const = default;
};

// 传输后端抽象接口：在上游 Mooncake Transfer_Engine 之上实现（任务 3.2）。
// 所有方法以 Result<T>/Status 报告成败，**不跨边界抛异常**（design.md「Error Handling」）。
class ITransferBackend {
 public:
  virtual ~ITransferBackend() = default;

  // 零拷贝：注册一段已存在的缓冲；payload 永不经中转缓冲拷贝（需求 3.3）。
  // 调用方拥有该缓冲，注册关系由适配器跟踪（配合 RAII 释放，需求 7.3）。
  // on_gpu=true 表示显存缓冲（location="cuda:N"），用于 GPUDirect（需求 3.2）。
  virtual Result<BufferHandle> RegisterBuffer(void* addr, std::size_t len,
                                              bool on_gpu) = 0;

  // 注销由 RegisterBuffer 注册的缓冲（需求 7.3：与注册严格配对）。
  virtual Status DeregisterBuffer(const BufferHandle& handle) = 0;

  // 路径选择：RDMA/GPUDirect 可用时优先选用，否则回退 TCP（需求 3.1/3.2/3.7）。
  virtual TransportPath SelectPath(const TransferRequest& request) const = 0;

  // 异步提交：立即返回 pending future，调用方在传输完成前即可继续执行（需求 3.4）。
  // future 兑现为成功回执或携带失败块键的错误状态（需求 3.6）。
  virtual std::future<Result<TransferReceipt>> SubmitAsync(
      TransferRequest request) = 0;
};

}  // namespace project::adapter

#endif  // PROJECT_ADAPTER_TRANSFER_ENGINE_ADAPTER_H_
