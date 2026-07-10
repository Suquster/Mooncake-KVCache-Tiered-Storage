// =============================================================================
// src/adapter/mooncake_transfer_backend.cpp —— 上游 Transfer Engine 生产接线
// =============================================================================
#include "adapter/mooncake_transfer_backend.h"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

// 唯一允许 include 上游头文件的位置（适配器边界，需求 6.2）。
#include "transfer_engine_c.h"

namespace project::adapter {

namespace {

// 上游 transfer_status.status 轮询间隔与超时。
constexpr auto kPollInterval = std::chrono::microseconds(50);
constexpr auto kTransferTimeout = std::chrono::seconds(30);

std::string PathToLocation(const MooncakeBackendConfig& config, bool on_gpu) {
  return on_gpu ? config.gpu_location : std::string("cpu:0");
}

}  // namespace

Result<std::unique_ptr<MooncakeTransferBackend>>
MooncakeTransferBackend::Create(const MooncakeBackendConfig& config) {
  transfer_engine_t engine = createTransferEngine(
      config.metadata_conn_string.c_str(), config.local_server_name.c_str(),
      config.ip_or_host_name.c_str(), config.rpc_port,
      /*auto_discover=*/0);
  if (engine == nullptr) {
    return Status::Make(StatusCode::kTransferFailed,
                        "createTransferEngine 失败：无法初始化上游引擎");
  }

  // RDMA 可用则优先安装（需求 3.1），失败时回退 TCP（需求 3.7）。
  bool rdma_installed = false;
  if (config.try_rdma) {
    rdma_installed = installTransport(engine, "rdma", nullptr) != nullptr;
  }
  if (!rdma_installed && installTransport(engine, "tcp", nullptr) == nullptr) {
    destroyTransferEngine(engine);
    return Status::Make(StatusCode::kTransferFailed,
                        "installTransport 失败：RDMA 与 TCP 均不可用");
  }

  // 目标段：向元数据服务解析目标节点的段句柄；目标地址即段内偏移。
  const std::string segment_name = config.remote_segment_name.empty()
                                       ? config.local_server_name
                                       : config.remote_segment_name;
  const segment_id_t segment = openSegment(engine, segment_name.c_str());
  if (segment < 0) {
    destroyTransferEngine(engine);
    return Status::Make(StatusCode::kTransferFailed,
                        "openSegment 失败：segment=" + segment_name);
  }

  return std::unique_ptr<MooncakeTransferBackend>(
      new MooncakeTransferBackend(engine, config, rdma_installed, segment));
}

MooncakeTransferBackend::MooncakeTransferBackend(void* engine,
                                                 MooncakeBackendConfig config,
                                                 bool rdma_installed,
                                                 std::int32_t segment)
    : engine_(engine),
      config_(std::move(config)),
      rdma_installed_(rdma_installed),
      segment_(segment) {}

MooncakeTransferBackend::~MooncakeTransferBackend() {
  {
    // RAII 兜底：仍存活的注册全部注销（需求 7.3）。
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [reg_id, addr] : live_registrations_) {
      (void)unregisterLocalMemory(engine_, addr);
    }
    live_registrations_.clear();
  }
  (void)closeSegment(engine_, segment_);
  destroyTransferEngine(engine_);
}

Result<BufferHandle> MooncakeTransferBackend::RegisterBuffer(void* addr,
                                                             std::size_t len,
                                                             bool on_gpu) {
  const std::string location = PathToLocation(config_, on_gpu);
  if (registerLocalMemory(engine_, addr, len, location.c_str(),
                          /*remote_accessible=*/1) != 0) {
    return Status::Make(StatusCode::kAddressNotRegistered,
                        "registerLocalMemory 失败：location=" + location);
  }
  std::lock_guard<std::mutex> lock(mu_);
  BufferHandle handle{addr, len, next_reg_id_++, on_gpu};
  live_registrations_.emplace(handle.reg_id, addr);
  return handle;
}

Status MooncakeTransferBackend::DeregisterBuffer(const BufferHandle& handle) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (live_registrations_.erase(handle.reg_id) == 0) {
      return Status::Make(StatusCode::kAddressNotRegistered,
                          "DeregisterBuffer：未知或已注销的 reg_id");
    }
  }
  if (unregisterLocalMemory(engine_, handle.addr) != 0) {
    return Status::Make(StatusCode::kTransferFailed,
                        "unregisterLocalMemory 失败");
  }
  return Status::Ok();
}

TransportPath MooncakeTransferBackend::SelectPath(
    const TransferRequest& request) const {
  if (!rdma_installed_) {
    return TransportPath::kTcp;  // 仅 TCP 环境的回退路径（需求 3.7）
  }
  if (request.src.on_gpu || request.dst.on_gpu) {
    return TransportPath::kGpuDirect;  // GPU↔NIC 直通（需求 3.2）
  }
  return TransportPath::kRdma;
}

std::future<Result<TransferReceipt>> MooncakeTransferBackend::SubmitAsync(
    TransferRequest request) {
  const TransportPath path = SelectPath(request);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (live_registrations_.count(request.src.reg_id) == 0 ||
        live_registrations_.count(request.dst.reg_id) == 0) {
      std::promise<Result<TransferReceipt>> promise;
      promise.set_value(Status::Make(StatusCode::kAddressNotRegistered,
                                     "SubmitAsync：源或目标缓冲未注册"));
      return promise.get_future();
    }
  }

  // 异步语义（需求 3.4）：submitTransfer 本身立即返回；完成状态经
  // getTransferStatus 在后台线程轮询兑现 future。
  return std::async(std::launch::async, [this, request, path]()
                        -> Result<TransferReceipt> {
    const batch_id_t batch = allocateBatchID(engine_, 1);
    if (batch == INVALID_BATCH) {
      return Status::Make(StatusCode::kTransferFailed,
                          "allocateBatchID 失败");
    }
    // 上游语义：target_id 为 openSegment 解析的目标段，target_offset 为目标段内
    // 的远端虚拟地址（已经 registerLocalMemory 暴露）。零拷贝：负载直接从
    // 已注册源缓冲 DMA 到远端地址，不经中转拷贝（需求 3.3）。
    transfer_request_t entry{};
    entry.opcode = OPCODE_WRITE;
    entry.source = request.src.addr;
    entry.target_id = segment_;
    entry.target_offset = reinterpret_cast<std::uint64_t>(request.dst.addr);
    entry.length = request.src.length;
    if (submitTransfer(engine_, batch, &entry, 1) != 0) {
      (void)freeBatchID(engine_, batch);
      return Status::Make(
          StatusCode::kTransferFailed,
          "submitTransfer 失败：key.hash_id=" +
              std::to_string(request.key.hash_id));
    }

    const auto deadline = std::chrono::steady_clock::now() + kTransferTimeout;
    transfer_status_t status{};
    while (std::chrono::steady_clock::now() < deadline) {
      if (getTransferStatus(engine_, batch, 0, &status) != 0) {
        (void)freeBatchID(engine_, batch);
        return Status::Make(StatusCode::kTransferFailed,
                            "getTransferStatus 失败");
      }
      if (status.status == STATUS_COMPLETED) {
        (void)freeBatchID(engine_, batch);
        return TransferReceipt{request.key, path,
                               static_cast<std::size_t>(
                                   status.transferred_bytes)};
      }
      if (status.status == STATUS_FAILED || status.status == STATUS_TIMEOUT ||
          status.status == STATUS_CANCELED) {
        (void)freeBatchID(engine_, batch);
        // 失败携带失败块键（需求 3.6）。
        return Status::Make(
            StatusCode::kTransferFailed,
            "传输失败：key.hash_id=" + std::to_string(request.key.hash_id) +
                " path=" + TransportPathName(path) +
                " upstream_status=" + std::to_string(status.status));
      }
      std::this_thread::sleep_for(kPollInterval);
    }
    (void)freeBatchID(engine_, batch);
    return Status::Make(StatusCode::kTransferFailed,
                        "传输超时：key.hash_id=" +
                            std::to_string(request.key.hash_id));
  });
}

}  // namespace project::adapter

