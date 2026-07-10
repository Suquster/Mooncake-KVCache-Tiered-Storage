# 上游 Mooncake 架构分析（分析先行工件）

> **需求对应**：本文档满足 **需求 1.1**（描述 Transfer_Engine 公开接口、支持的传输协议、数据流动）
> 与 **需求 1.3**（记录锁定的上游 commit 哈希与版本作为分析基线）。
> Store_Layer 接口、键值对象模型、多层缓存行为与扩展点枚举（需求 1.2 / 1.4 / 1.5）已由任务 2.2 在
> 本文档「第二部分：Store_Layer 分析」章节（§6–§11）补充完成。本文档是「分析先行门禁」的核心工件：
> 在锁定 Tiered_Storage_Manager、Data_Path、Scheduler 的设计之前必须先完成（需求 1.5），门禁已签收
> （GATE: PASS，见 §11.5）。

---

## 0. 分析基线（锁定的上游版本，需求 1.3）

本分析严格基于以下锁定的上游 Mooncake 版本，所有接口签名、协议清单与数据流均直接取自该
commit 下 `third_party/mooncake/` 子模块中的**真实头文件**（而非外部文档或推测）。

| 项目 | 值 |
|---|---|
| 上游仓库 | `https://github.com/kvcache-ai/Mooncake.git` |
| 锁定 commit 哈希 | `356d99fb28746d274241b6792c2f7c2fe17e3b29` |
| 发布版本 | `0.3.6.post1` |
| 对应 Git tag | `v0.3.6.post1` |
| 开源许可 | Apache-2.0 |
| 子模块路径 | `third_party/mooncake` |
| 锁定时间（UTC） | `2026-06-10T00:00:00Z` |

> 上述值与仓库根目录的 `mooncake.lock` 完全一致；`mooncake.lock` 是构建期版本校验与基准报告
> 溯源（需求 5.5）的唯一可信来源。本文档引用的所有头文件路径均相对于
> `third_party/mooncake/`，并在该 commit 下经过实际查阅核对。

### 本次分析实际查阅的真实头文件清单

| 头文件路径（相对 `third_party/mooncake/`） | 用途 |
|---|---|
| `mooncake-transfer-engine/include/transfer_engine.h` | `TransferEngine` 顶层公开类，引擎生命周期、内存注册、批量提交、状态查询、通知 |
| `mooncake-transfer-engine/include/transport/transport.h` | `Transport` 抽象基类，`TransferRequest`/`TransferStatus`/`Slice`/`TransferTask`/`BatchDesc` 等核心数据结构 |
| `mooncake-transfer-engine/include/multi_transport.h` | `MultiTransport` 多后端管理与 `selectTransport` 路径选择 |
| `mooncake-transfer-engine/include/topology.h` | `Topology`/`TopologyMatrix` 拓扑感知与设备优先级矩阵 |
| `mooncake-transfer-engine/include/transfer_engine_c.h` | C ABI 接口（FFI 绑定基线，供 Python/Rust/Go 使用） |
| `mooncake-transfer-engine/include/common/base/status.h` | `Status` 错误模型（提交/完成的返回语义） |
| `mooncake-transfer-engine/include/transport/rdma_transport/rdma_transport.h` | RDMA / GPUDirect RDMA 后端 |
| `mooncake-transfer-engine/include/transport/tcp_transport/tcp_transport.h` | TCP 回退后端 |
| `mooncake-transfer-engine/include/transport/nvmeof_transport/nvmeof_transport.h` | NVMe-oF（cuFile / GPUDirect Storage）后端 |
| `mooncake-transfer-engine/include/transport/nvlink_transport/nvlink_transport.h` | NVLink / 共享显存后端 |
| `doc/en/transfer-engine.md` | 上游官方说明（用于交叉印证语义，非签名来源） |

---

## 1. Transfer_Engine 概述（需求 1.1）

Mooncake Transfer Engine 是一个**高性能、零拷贝**的数据搬运库，围绕两个核心抽象构建：

- **Segment（段）**：一段可被远端读写的连续地址空间。分两类：
  - **RAM Segment**：由 DRAM 或显存（VRAM）提供的非持久化存储；
  - **NVMeof Segment**：由 NVMe-oF 提供的持久化存储（每个文件对应一个段）。
- **BatchTransfer（批量传输）**：封装一组操作请求，负责在一个 Segment 的若干非连续数据空间与
  另一组 Segment 的对应空间之间同步数据，双向支持 READ/WRITE，语义上等价于一种**异步、更灵活
  的 AllScatter / AllGather**。

每个客户端进程对应一个 `TransferEngine` 实例；该实例内部统一管理跨多线程、多网卡的高速传输，
并按拓扑自动选择后端 `Transport`。不同后端（TCP / RDMA / GPUDirect / NVMe-oF / NVLink）实现各自
的搬运逻辑，但对上层暴露**统一的批量提交 + 异步状态查询**接口。

本项目（Project）的 `Data_Path` 与 `src/adapter/transfer_engine_adapter.*` 将**仅**在适配器层
include 上述上游头文件，把这一统一接口收敛为项目自有的 `ITransferBackend` 抽象，从而隔离上游
API 演进对其余模块的影响。

---

## 2. Transfer_Engine 公开接口（需求 1.1）

以下签名直接摘自锁定 commit 下的真实头文件，并标注来源。

### 2.1 顶层类 `mooncake::TransferEngine`

来源：`mooncake-transfer-engine/include/transfer_engine.h`

```cpp
namespace mooncake {
// 顶层类型别名：Transfer_Engine 把 Transport 的核心类型再导出
using TransferRequest    = Transport::TransferRequest;
using TransferStatus     = Transport::TransferStatus;
using TransferStatusEnum = Transport::TransferStatusEnum;
using SegmentHandle      = Transport::SegmentHandle;   // = uint64_t
using SegmentID          = Transport::SegmentID;       // = uint64_t
using BatchID            = Transport::BatchID;          // = uint64_t
using BufferEntry        = Transport::BufferEntry;

class TransferEngine {
 public:
  explicit TransferEngine(bool auto_discover = false);
  TransferEngine(bool auto_discover, const std::vector<std::string>& filter);
  ~TransferEngine();   // 析构调用 freeEngine()，回收全部资源并删除全局元数据

  // —— 生命周期 ——
  int init(const std::string& metadata_conn_string,
           const std::string& local_server_name,
           const std::string& ip_or_host_name = "",
           uint64_t rpc_port = 12345);
  int freeEngine();

  // —— 传输后端安装/卸载（installTransport 仅用于测试场景显式安装）——
  Transport* installTransport(const std::string& proto, void** args);
  int        uninstallTransport(const std::string& proto);
  Transport* getTransport(const std::string& proto);

  // —— 段（Segment）管理 ——
  SegmentHandle openSegment(const std::string& segment_name);
  int           closeSegment(SegmentHandle handle);
  int           removeLocalSegment(const std::string& segment_name);
  int           syncSegmentCache(const std::string& segment_name = "");

  // —— 本地内存注册（零拷贝的前提：把缓冲区注册为可被 RDMA 读写的 MR）——
  int registerLocalMemory(void* addr, size_t length,
                          const std::string& location = kWildcardLocation,
                          bool remote_accessible = true,
                          bool update_metadata = true);
  int unregisterLocalMemory(void* addr, bool update_metadata = true);
  int registerLocalMemoryBatch(const std::vector<BufferEntry>& buffer_list,
                               const std::string& location);
  int unregisterLocalMemoryBatch(const std::vector<void*>& addr_list);

  // —— 批量传输（核心数据搬运 API）——
  BatchID allocateBatchID(size_t batch_size);
  Status  freeBatchID(BatchID batch_id);
  Status  submitTransfer(BatchID batch_id,
                         const std::vector<TransferRequest>& entries);
  Status  submitTransferWithNotify(BatchID batch_id,
                                   const std::vector<TransferRequest>& entries,
                                   TransferMetadata::NotifyDesc notify_msg);
  Status  getTransferStatus(BatchID batch_id, size_t task_id,
                            TransferStatus& status);
  Status  getBatchTransferStatus(BatchID batch_id, TransferStatus& status);

  // —— 带外通知（completion 之后随状态查询触发发送）——
  int getNotifies(std::vector<TransferMetadata::NotifyDesc>& notifies);
  int sendNotifyByID(SegmentID target_id, TransferMetadata::NotifyDesc notify_msg);
  int sendNotifyByName(std::string remote_agent, TransferMetadata::NotifyDesc notify_msg);

  // —— 拓扑/诊断 ——
  bool                      checkOverlap(void* addr, uint64_t length);
  int                       numContexts() const;
  std::shared_ptr<Topology> getLocalTopology() const;
  std::string               getLocalIpAndPort();
  int                       getRpcPort();
};
}  // namespace mooncake
```

**接口要点（对本项目设计的影响）**：

1. **零拷贝的前提是显式内存注册**。`registerLocalMemory` 把一段 DRAM/VRAM 注册为可被
   （GPUDirect）RDMA 直接读写的 Memory Region；`TransferRequest::source` 必须事先注册。
   本项目 `ITransferBackend::RegisterBuffer / DeregisterBuffer`（设计文档）正是对这对接口的封装，
   并以 RAII（`ScopedRegistration`）保证 `register` 与 `unregister` 配对（需求 7.3）。
2. **`location` 参数驱动网卡亲和性**。如 `"cuda:0"`、`"cpu:0"` 或通配 `"*"`，与拓扑优先级矩阵
   匹配后决定首选网卡，这是 GPUDirect RDMA「走本地 PCIe Switch」的关键。
3. **异步提交 + 轮询完成**。`submitTransfer` 把请求异步投递到后台线程池后立即返回，调用方通过
   `getTransferStatus` / `getBatchTransferStatus` 轮询完成状态——这正对应本项目
   `ITransferBackend::SubmitAsync` 返回 future、调用方先继续执行（需求 3.4）的语义基础。
4. **`auto_discover`**：构造时为 `true` 则自动发现 CPU/CUDA/RDMA 设备拓扑并自动安装 Transport；
   为 `false`（仅测试）时需手动 `installTransport`。

### 2.2 抽象基类 `mooncake::Transport` 与核心数据结构

来源：`mooncake-transfer-engine/include/transport/transport.h`

```cpp
class Transport {
 public:
  using SegmentID   = uint64_t;
  using SegmentHandle= SegmentID;
  using BatchID     = uint64_t;
  static const BatchID INVALID_BATCH_ID = UINT64_MAX;

  // 单条传输请求：源地址 -> <目标段, 目标偏移>，或反向，按 opcode 决定方向
  struct TransferRequest {
    enum OpCode { READ, WRITE };
    OpCode    opcode;
    void*     source;            // 本地已注册的 DRAM/VRAM 缓冲区
    SegmentID target_id;         // openSegment 得到；可指向本地/远端 DRAM/VRAM/NVMeof
    uint64_t  target_offset;     // 目标段内偏移（远端虚拟地址或文件偏移）
    size_t    length;
    int       advise_retry_cnt = 0;
  };

  enum TransferStatusEnum {
    WAITING, PENDING, INVALID, CANCELED, COMPLETED, TIMEOUT, FAILED
  };
  struct TransferStatus {
    TransferStatusEnum s;
    size_t             transferred_bytes;   // 已成功搬运字节数（下界，非精确值）
  };

  // —— 后端必须实现的批量接口 ——
  virtual BatchID allocateBatchID(size_t batch_size);
  virtual Status  freeBatchID(BatchID batch_id);
  virtual Status  submitTransfer(BatchID batch_id,
                                 const std::vector<TransferRequest>& entries) = 0;
  virtual Status  submitTransferTask(const std::vector<TransferTask*>& task_list);
  virtual Status  getTransferStatus(BatchID batch_id, size_t task_id,
                                    TransferStatus& status) = 0;

  struct BufferEntry { void* addr; size_t length; };

 private:
  // 每个后端实现的内存注册（私有，经由 TransferEngine 转发）
  virtual int registerLocalMemory(void* addr, size_t length,
                                  const std::string& location,
                                  bool remote_accessible,
                                  bool update_metadata = true) = 0;
  virtual int unregisterLocalMemory(void* addr, bool update_metadata = true) = 0;
  virtual int registerLocalMemoryBatch(const std::vector<BufferEntry>& buffer_list,
                                       const std::string& location) = 0;
  virtual int unregisterLocalMemoryBatch(const std::vector<void*>& addr_list) = 0;
  virtual const char* getName() const = 0;
};
```

**内部执行模型**（同一头文件，影响异步流水线与资源纪律设计）：

- **`Slice`（切片）**：单条 `TransferRequest` 在内部可按 `MC_SLICE_SIZE`（默认 >64KB 时）拆为多个
  `Slice`，每个切片可走不同网卡路径以聚合带宽。`Slice` 是一个 `union`，按后端分别携带 `rdma` /
  `local` / `tcp` / `nvmeof` / `cxl` / `hccl` / `ascend_direct` 字段——**这正是「支持的传输协议」
  集合在代码层面的直接证据**。`Slice::markSuccess()` / `markFailed()` 以原子加更新所属
  `TransferTask` 的计数。
- **`TransferTask`**：聚合一组 `Slice`，维护 `slice_count` / `success_slice_count` /
  `failed_slice_count` / `transferred_bytes` / `is_finished`；析构时把切片归还
  `ThreadLocalSliceCache`。
- **`ThreadLocalSliceCache`**：线程局部的切片对象池（容量 `kLazyDeleteSliceCapacity = 4096`），
  析构时若 `allocated_ != freed_` 会打印 slice 泄漏告警——印证上游对**资源守恒**的重视，与本项目
  需求 7.3 的「获取数 == 释放数」不变量一致。
- **`BatchDesc`**：描述一个 batch（`id` / `batch_size` / `task_list` / `context` /
  `start_timestamp`），由 `batch_desc_lock_`（`RWSpinlock`）保护。

### 2.3 错误/完成模型 `mooncake::Status`

来源：`mooncake-transfer-engine/include/common/base/status.h`

`submitTransfer` / `freeBatchID` / `getTransferStatus` 等返回 `Status`（设计借鉴 RocksDB）。
`Status::Code` 枚举提供细粒度错误码，与「传输失败需携带失败信息」（需求 3.6）相关的有：

| Code | 含义（与本项目错误处理映射） |
|---|---|
| `kOk = 0` | 成功 |
| `kInvalidArgument = 1` | 参数非法 |
| `kTooManyRequests = 2` | 超出 batch 容量/背压（对应异步流水线背压设计，需求 3.4） |
| `kAddressNotRegistered = 3` | 源地址未注册（零拷贝前置条件不满足，需求 3.3） |
| `kBatchBusy = 4` | batch 仍有未完成任务时被释放 |
| `kDeviceNotFound = 6` | 设备缺失（与能力探测/路径回退相关，需求 3.7） |
| `kNotSupportedTransport = 8` | 选定协议不可用（路径选择回退依据，需求 3.7） |
| `kEndpoint = 201` / `kContext = 202` | RDMA 端点/上下文故障（多网卡故障切换相关） |
| `kMemory = 302` | 内存相关错误 |
| `kNotImplemented = 999` | 后端未实现该能力 |

`Status` 同时携带 `code()` 与 `message()`，本项目 `Data_Path` 在故障路径上会把上游 `Status` 的
`code/message` 连同**失败块键 `BlockKey`** 一起封装进 `TransferError{failing_key, path, reason}`
（需求 3.6）。注意：上游底层 `Transport` 私有接口仍以 POSIX 风格 `int`（0 成功 / -1 失败并设
`errno`）返回，`Status` 是上层统一封装。

### 2.4 C ABI 接口（FFI 基线）

来源：`mooncake-transfer-engine/include/transfer_engine_c.h`

上游提供完整 C ABI（`extern "C"`），是 Python / Rust / Go 绑定的基线，关键导出：

```c
transfer_engine_t createTransferEngine(const char* metadata_conn_string,
                                       const char* local_server_name,
                                       const char* ip_or_host_name,
                                       uint64_t rpc_port, int auto_discover);
void              destroyTransferEngine(transfer_engine_t engine);
transport_t       installTransport(transfer_engine_t engine, const char* proto, void** args);
int               registerLocalMemory(transfer_engine_t, void* addr, size_t length,
                                       const char* location, int remote_accessible);
int               unregisterLocalMemory(transfer_engine_t, void* addr);
segment_id_t      openSegment(transfer_engine_t, const char* segment_name);
batch_id_t        allocateBatchID(transfer_engine_t, size_t batch_size);
int               submitTransfer(transfer_engine_t, batch_id_t, struct transfer_request* entries, size_t count);
int               getTransferStatus(transfer_engine_t, batch_id_t, size_t task_id, struct transfer_status* status);
int               freeBatchID(transfer_engine_t, batch_id_t);
```

C 层 `struct segment_desc` 显式区分 `rdma` 与 `nvmeof` 两类描述（`int type; // RDMA / NVMeoF`），
其 `union` 字段也印证了协议分型。本项目 `vLLM_Adapter`（Python）经由 pybind11 绑定与该 C/C++
接口对接，**不修改上游源码**（需求 6.2）。

---

## 3. 支持的传输协议（需求 1.1）

Transfer_Engine 采用「统一接口 + 多后端 `Transport`」结构，由 `MultiTransport` 按拓扑安装并
路由。锁定 commit 下，每个协议都有独立的 `Transport` 子类（头文件实证如下）。

| 协议 | `Transport` 子类 / `getName()` | 头文件来源 | 适用场景与要点 |
|---|---|---|---|
| **TCP** | `TcpTransport` → `"tcp"` | `transport/tcp_transport/tcp_transport.h` | 仅 TCP 网络环境的**回退路径**（需求 3.7）；本地 DRAM ↔ 远端 DRAM；内部以 `worker()` 线程驱动 `startTransfer(Slice*)` |
| **RDMA / RoCE** | `RdmaTransport` → `"rdma"` | `transport/rdma_transport/rdma_transport.h` | 本地 DRAM/VRAM ↔ 远端 DRAM 的高性能路径；多网卡池化与重试；`onSetupRdmaConnections` 按需握手建链；依赖 `<infiniband/verbs.h>` |
| **GPUDirect RDMA** | 同 `RdmaTransport` | 同上 + `registerLocalMemory(location="cuda:N")` | **并非独立后端**：在 RDMA 后端上注册显存（`location` 为 `cuda:*`）即获得 GPU↔NIC 直通，绕过主机内存中转（需求 3.2）。拓扑矩阵确保走本地 PCIe Switch |
| **NVMe-oF（GPUDirect Storage / cuFile）** | `NVMeoFTransport` → `"nvmeof"` | `transport/nvmeof_transport/nvmeof_transport.h` | 经 PCIe 在 NVMe 文件与 DRAM/VRAM 间直传，不经过 CPU、实现零拷贝；基于 `cuFile`（`cufile_context.h` / `cufile_desc_pool.h`），每文件一个段 |
| **NVLink / 共享显存** | `NvlinkTransport` → `"nvlink"` | `transport/nvlink_transport/nvlink_transport.h` | 同主机/可达域内 GPU 间共享显存直传；`allocatePinnedLocalMemory`、`relocateSharedMemoryAddress` 重映射；依赖 `<cuda_runtime.h>`，可选 fabric memory |
| **CXL** | `CxlTransport` | `transport/cxl_transport/cxl_transport.h` | CXL 内存语义传输（`Slice` union 含 `cxl.dest_addr` 字段佐证） |
| **Ascend（异构 RDMA / HCCL / 直传）** | `heterogeneous_rdma_transport.h` 等 | `transport/ascend_transport/` | 昇腾 NPU 异构传输（`Slice` union 含 `hccl` / `ascend_direct` 字段佐证），非本项目 NVIDIA 目标平台必需 |

> **`Slice` union 的代码级佐证**：`transport.h` 中 `Slice` 的 `union` 同时定义了 `rdma` /
> `local`（本地 memcpy/cudaMemcpy）/ `tcp` / `nvmeof` / `cxl` / `hccl` / `ascend_direct` 七种
> 后端专属字段，这是「支持的传输协议」最直接、最权威的源码证据。

**本地直拷优化**：当目标段实际位于本地 DRAM/VRAM 时，引擎直接使用 `memcpy` / `cudaMemcpy`
（`Slice::local` 分支），不经网络后端。

### 3.1 协议路径选择（与需求 3.1 / 3.7 对接）

来源：`mooncake-transfer-engine/include/multi_transport.h`

`MultiTransport` 持有 `std::map<std::string, std::shared_ptr<Transport>> transport_map_`，并提供：

```cpp
Transport* installTransport(const std::string& proto, std::shared_ptr<Topology> topo);
Transport* getTransport(const std::string& proto);
std::vector<Transport*> listTransports();
private:
  Status selectTransport(const TransferRequest& entry, Transport*& transport); // 按请求选后端
```

`TransferEngine(auto_discover=true)` 会自动发现 CPU/CUDA/RDMA 拓扑并安装相应 `Transport`；
`selectTransport` 依据请求的源/目标内存属性挑选后端。本项目 `ITransferBackend::SelectPath`
（设计文档：RDMA/GPUDirect 可用则优先、否则 TCP 回退，需求 3.1/3.2/3.7）正是建立在这一机制之上：
适配器层探测可用后端（`getTransport("rdma") != nullptr` 等）并据此把项目侧
`TransportPath::{kRdma,kGpuDirect,kTcp}` 映射到上游协议名。

### 3.2 拓扑感知路径选择（多网卡带宽聚合与故障切换）

来源：`mooncake-transfer-engine/include/topology.h`

```cpp
using TopologyMatrix = std::unordered_map<std::string /*storage type*/, TopologyEntry>;
struct TopologyEntry {
  std::string name;
  std::vector<std::string> preferred_hca;   // 首选网卡列表
  std::vector<std::string> avail_hca;       // 备选网卡列表
};
class Topology {
 public:
  int discover(const std::vector<std::string>& filter);   // 自动发现拓扑
  int parse(const std::string& topology_json);
  int selectDevice(const std::string storage_type, int retry_count = 0);
  int selectDevice(const std::string storage_type, std::string_view hint, int retry_count = 0);
  TopologyMatrix getMatrix() const;
  const std::vector<std::string>& getHcaList() const;
};
```

每个节点生成拓扑矩阵并在集群广播，把网卡按内存类型分为 **preferred / secondary** 两档：

- 正常情况选 preferred 网卡，使 RDMA 操作落在本地 NUMA 或经本地 PCIe Switch 的 GPUDirect RDMA；
- 故障时可同时启用两档网卡（多网卡故障切换）；
- 单请求超过 64KB 时拆为多个 `Slice`，不同切片可走不同网卡路径以**聚合带宽**。

这一拓扑模型为本项目 `Scheduler` 的「故障切换 / 负载均衡」（需求 4.4 / 4.5）提供了底层支撑：上游
已在**传输层**做网卡级故障切换，本项目在**控制面**做节点级故障切换与放置均衡，两者分层互补。

---

## 4. 数据流动：register → submit → complete（需求 1.1）

Transfer_Engine 的搬运遵循「**注册 → 提交 → 完成**」三阶段异步流程。下面以真实接口逐阶段说明，
并标注与本项目设计的对接点。

### 阶段一：注册（Register）—— 建立零拷贝前提

```cpp
engine.init(metadata_conn_string, local_server_name);            // 连接元数据服务，建立本进程 RAM Segment
SegmentHandle seg = engine.openSegment(remote_server_name);      // 引用远端段，拿到 SegmentID
engine.registerLocalMemory(buf, len, /*location=*/"cuda:0",      // 把本地 DRAM/VRAM 注册为可 RDMA 读写的 MR
                           /*remote_accessible=*/true);
```

- `init` 用全局唯一的 `local_server_name` 创建本进程的 RAM Segment，并向元数据服务
  （etcd / redis / http）注册可达地址与 RPC 端口。
- `openSegment` 按段名解析出 `SegmentID`（RAM 段名即对端 `local_server_name`；NVMeof 段名为文件
  唯一标识）。
- `registerLocalMemory` 是**零拷贝的核心前提**：注册后 `TransferRequest::source` 才能被 RDMA
  直接读写，避免经中转缓冲区拷贝（需求 3.3）。`location` 决定网卡亲和；`remote_accessible` 决定
  是否可被远端引用。
- **本项目对接**：`ITransferBackend::RegisterBuffer/DeregisterBuffer` 封装这对接口，
  `BufferHandle{addr,length,reg_id,on_gpu}` 持有注册句柄，`ScopedRegistration`（RAII）保证在所有
  路径（含错误路径）上 register/unregister 配对，满足资源守恒（需求 7.3）。

### 阶段二：提交（Submit）—— 异步批量投递

```cpp
BatchID batch = engine.allocateBatchID(batch_size);              // 分配 batch，限定最大并发请求数
std::vector<TransferRequest> reqs = {
  { TransferRequest::WRITE, /*source=*/buf, /*target_id=*/seg,
    /*target_offset=*/off, /*length=*/len, /*advise_retry_cnt=*/0 },
  // ... 至多 batch_size 条
};
Status s = engine.submitTransfer(batch, reqs);                   // 异步投递到后台线程池后立即返回
// 或带通知：engine.submitTransferWithNotify(batch, reqs, notify_msg);
```

- `allocateBatchID(batch_size)` 限定同一 batch 下最多可提交 `batch_size` 条 `TransferRequest`。
- `submitTransfer` 把请求**异步**提交到后台线程池后**立即返回** `Status`（不阻塞等待完成）。
  内部 `MultiTransport::selectTransport` 为每条请求挑选后端，再由后端把请求拆成 `Slice` 投递
  （RDMA 走 QP，TCP 走 worker 线程，NVMe-oF 走 cuFile 批，NVLink 走共享显存重映射）。
- 超出 batch 容量会返回 `kTooManyRequests`/`kBatchBusy` 类错误——这是**背压**的天然来源。
- **本项目对接**：`ITransferBackend::SubmitAsync(TransferRequest)` 返回
  `std::future<Result<TransferReceipt>>`，调用方（`Scheduler` / `Prefetch_Engine`）在传输完成前
  即可继续执行（需求 3.4）；`Data_Path` 的有界提交队列在上游容量上限之上再叠加一层背压。

### 阶段三：完成（Complete）—— 轮询状态与通知

```cpp
TransferStatus st;
do {
  engine.getTransferStatus(batch, /*task_id=*/0, st);            // 轮询单条任务状态
} while (st.s == TransferStatusEnum::WAITING);
// st.s ∈ {COMPLETED, FAILED, INVALID, ...}; st.transferred_bytes 为已传字节数（下界）
engine.freeBatchID(batch);                                       // 回收 batch（仍有未完成任务则拒绝）
```

- `getTransferStatus(batch, task_id, &st)` 查询单条请求状态；`getBatchTransferStatus` 查询整个
  batch 的聚合状态。状态枚举见 §2.2（`WAITING/PENDING/INVALID/CANCELED/COMPLETED/TIMEOUT/FAILED`）。
  完成判定：返回值表示完成与否，`COMPLETED` 表示成功，`FAILED` 表示重试后仍失败。
- 内部由 `Slice::markSuccess()/markFailed()` 原子累加 `TransferTask` 的成功/失败计数与
  `transferred_bytes`，所有切片就绪后 `TransferTask::is_finished` 置位。
- **带外通知**：若用 `submitTransferWithNotify` 提交，引擎在该 batch 达到 `COMPLETED` 时（于
  `getTransferStatus`/`getBatchTransferStatus` 内部）触发 `sendNotifyByID` 把 `NotifyDesc` 发往
  目标段；接收方用 `getNotifies` 收取。这为本项目跨节点完成事件通知提供了原语。
- `freeBatchID` 回收 batch；若仍有未完成请求则拒绝（避免悬空）。
- **本项目对接**：`Data_Path` 把 future 的就绪映射到上游 `COMPLETED`；失败时读取上游 `Status`
  的 `code/message`，连同失败 `BlockKey` 封装为 `TransferError`（需求 3.6）。

### 数据流动时序图

```
 调用方(Data_Path/Adapter)        TransferEngine                MultiTransport / Transport 后端
        │                              │                                  │
        │ init / openSegment           │                                  │
        ├─────────────────────────────▶│ 连元数据服务, 建 RAM Segment       │
        │ registerLocalMemory(buf,loc) │                                  │
        ├─────────────────────────────▶│ 注册 MR (零拷贝前提, 网卡亲和)      │
        │                              │                                  │
        │ allocateBatchID(n)           │                                  │
        ├─────────────────────────────▶│                                  │
        │ submitTransfer(batch, reqs)  │  selectTransport(req)            │
        ├─────────────────────────────▶├─────────────────────────────────▶│ 拆 Slice, 投递
        │   ◀── 立即返回 Status (异步) ──┤                                  │  (RDMA QP / TCP worker /
        │                              │                                  │   cuFile / NVLink shm)
        │ getTransferStatus(...) 轮询   │                                  │ markSuccess/markFailed
        ├─────────────────────────────▶│  聚合 TransferTask 计数            │  原子累加 transferred_bytes
        │   ◀── COMPLETED / FAILED ─────┤  (完成则触发 NotifyDesc 发送)      │
        │ freeBatchID(batch)           │                                  │
        ├─────────────────────────────▶│                                  │
```

---

## 5. 对本项目设计的输入（小结，衔接需求 1.4 / 1.5）

下表把上游 Transfer_Engine 的真实能力映射到本项目设计中的对接点（扩展点完整枚举由任务 2.2 补全）：

| 上游真实接口 / 机制 | 本项目设计对接点 | 关联需求 |
|---|---|---|
| `registerLocalMemory` / `unregisterLocalMemory`（+ `location` 网卡亲和） | `ITransferBackend::RegisterBuffer/DeregisterBuffer`、`BufferHandle`、`ScopedRegistration`（RAII） | 3.3、7.3 |
| 多后端 `Transport`（rdma/tcp/nvmeof/nvlink/...）+ `MultiTransport::selectTransport` | `ITransferBackend::SelectPath`、`TransportPath{kRdma,kGpuDirect,kTcp}` 能力探测与回退 | 3.1、3.2、3.7 |
| `allocateBatchID` + `submitTransfer`（异步立即返回） | `ITransferBackend::SubmitAsync` 返回 future + 有界提交队列背压 | 3.4 |
| `getTransferStatus` / `getBatchTransferStatus` + `Status` 错误码 | future 完成映射 + `TransferError{failing_key, path, reason}` | 3.6 |
| `submitTransferWithNotify` / `getNotifies`（带外通知） | 跨节点完成事件通知原语 | 4.x（调度协同） |
| `Topology` 拓扑矩阵 + 多网卡故障切换 | `Scheduler` 节点级故障切换/负载均衡的传输层基础 | 4.4、4.5 |
| C ABI（`transfer_engine_c.h`）/ pybind11 | `vLLM_Adapter` 经绑定对接，不改上游源码 | 6.1、6.2 |
| `mooncake.lock` 锁定 commit/version | 构建期版本校验 + 基准报告溯源 | 1.3、6.3、6.5、5.5 |

> **门禁状态**：本文档已完成 Transfer_Engine 面（公开接口、传输协议、register→submit→complete
> 数据流）的成文分析，并记录了锁定基线 commit `356d99fb28746d274241b6792c2f7c2fe17e3b29`
> （v0.3.6.post1）。**Store_Layer 面、扩展点完整枚举、以及对临时（provisional）组件接口签名的
> 对账与门禁签收已由任务 2.2 在下方「第二部分：Store_Layer 分析」章节完成（见 §6–§11）。
> 分析先行门禁结论为 GATE: PASS（详见 §11.5），下游 Tiered_Storage_Manager、Data_Path、
> Scheduler 设计据此锁定（需求 1.5）。**


---

# 第二部分：Store_Layer 分析（需求 1.2 / 1.4 / 1.5，由任务 2.2 补充）

> **需求对应**：本部分满足 **需求 1.2**（描述 Store_Layer 公开接口、键值对象模型、多层缓存行为）、
> **需求 1.4**（枚举 Tiered_Storage_Manager / Data_Path / Scheduler 将构建于其上的 Transfer_Engine
> 与 Store_Layer 扩展点，以及 Mooncake KV connector 接口面）、并完成 **需求 1.5** 的「分析先行门禁」
> 签收（对账设计文档中的临时组件接口签名，确认后方可锁定下游设计）。
>
> 所有签名均取自锁定 commit `356d99fb28746d274241b6792c2f7c2fe17e3b29`（v0.3.6.post1）下
> `third_party/mooncake/mooncake-store/` 的**真实头文件**，并在本部分逐处标注来源路径。

## 6. 本部分实际查阅的真实头文件清单（Store_Layer）

| 头文件路径（相对 `third_party/mooncake/`） | 用途 |
|---|---|
| `mooncake-store/include/client.h` | `mooncake::Client` 顶层公开类：Get/BatchGet/Query/Put/BatchPut/Remove/MountSegment/RegisterLocalMemory/IsExist 等对象级 K/V 接口 |
| `mooncake-store/include/types.h` | 对象模型基础类型：`ObjectKey`/`Version`/`SegmentId`/`Slice`/`Segment`/`ReplicaList`/`ErrorCode`、淘汰与租约常量 |
| `mooncake-store/include/replica.h` | `Replica`/`Replica::Descriptor`、`ReplicaType{MEMORY,DISK}`、`ReplicaStatus`、`ReplicateConfig`（多层/多副本对象模型核心证据） |
| `mooncake-store/include/allocator.h` | `BufferAllocatorBase`/`CachelibBufferAllocator`/`OffsetBufferAllocator`/`AllocatedBuffer`（内存层分配与 RAII 句柄） |
| `mooncake-store/include/allocation_strategy.h` | `AllocationStrategy`/`RandomAllocationStrategy`（best-effort 放置、`preferred_segment` 偏好、跨段冗余） |
| `mooncake-store/include/eviction_strategy.h` | `EvictionStrategy`/`LRUEvictionStrategy`/`FIFOEvictionStrategy`（淘汰策略抽象） |
| `mooncake-store/include/storage_backend.h` | `StorageBackend`（本地文件系统 / 3FS 持久化后端 = NVMe/SSD 层） |
| `mooncake-store/include/master_service.h` | `MasterService`：段挂载、容量查询、副本清单、PutStart/PutEnd、淘汰水位与租约（集群元数据控制面） |
| `mooncake-store/include/master_client.h` | `MasterClient`：客户端到 Master 的 RPC 代理（跨节点索引数据来源） |
| `mooncake-store/include/pybind_client.h` | `PyClient`/`BufferHandle`/`ResourceTracker`（Python 绑定层，零拷贝 `*_into`/`*_from` 接口） |
| `mooncake-integration/store/store_py.cpp` | `MooncakeDistributedStore` pybind 类（vLLM KV connector 实际对接的 Python 接口面） |

---

## 7. Store_Layer 概述（需求 1.2）

Mooncake Store（`mooncake-store`）是构建在 Transfer_Engine 之上的**分布式键值对象存储**，面向
KVCache 这类大对象的高吞吐 Put/Get。其核心特征：

- **以对象（Object）为单位**：每个对象由字符串键 `ObjectKey` 唯一标识，值为一段（可被切片的）
  连续字节缓冲。键到副本位置的映射由中心化的 **Master**（`MasterService`）维护，数据搬运由
  **Client**（`Client`）经 Transfer_Engine 在客户端与持有副本的段（Segment）之间零拷贝完成。
- **分离的控制面 / 数据面**：`MasterService` 是控制面（持有键→副本→段的全局元数据、做放置与淘汰
  决策）；`Client` + `TransferEngine` 是数据面（执行真正的零拷贝读写）。这与本项目「Scheduler 控制
  面 + Data_Path 数据面」的分层完全同构。
- **多层 / 多副本对象模型**：一个对象的每个副本（`Replica`）要么是**内存副本**（`ReplicaType::
  MEMORY`，落在 DRAM/VRAM 段上）、要么是**磁盘副本**（`ReplicaType::DISK`，落在 NVMe/SSD/3FS 文件
  上）。这正是 Store_Layer「多层缓存行为」在源码层的直接证据（详见 §9）。
- **段（Segment）= 一段被挂载的可分配/可远程访问内存**：客户端通过 `MountSegment(buffer, size)`
  把本地 DRAM/VRAM 贡献给集群作为内存层容量；Master 在这些段上做 best-effort 放置。

本项目（Project）的 `src/adapter/store_adapter.*` 将**仅**在适配器层 include 上述 Store_Layer
头文件，把对象级接口收敛为项目自有的 `IObjectStore` 抽象（`Put`/`Get`/`Exists`/`Locate`），从而把
KVCache_Block 键映射到 Store 的 `ObjectKey`，并隔离上游 API 演进（需求 6.2）。

---

## 8. Store_Layer 公开接口（需求 1.2）

### 8.1 顶层类 `mooncake::Client`（对象级 K/V API）

来源：`mooncake-store/include/client.h`。所有方法返回
`tl::expected<T, ErrorCode>`（成功携带 `T`，失败携带 `ErrorCode`，无跨边界异常）。

```cpp
namespace mooncake {
class Client {
 public:
  // —— 工厂：创建并初始化（内部建 TransferEngine + 连接 Master）——
  static std::optional<std::shared_ptr<Client>> Create(
      const std::string& local_hostname,
      const std::string& metadata_connstring,
      const std::string& protocol,                                  // "rdma" / "tcp"
      const std::optional<std::string>& device_names = std::nullopt,
      const std::string& master_server_entry = kDefaultMasterAddress);

  // —— 读路径（数据面，零拷贝进入调用方提供的 Slice）——
  tl::expected<void, ErrorCode> Get(const std::string& object_key,
                                    std::vector<Slice>& slices);
  std::vector<tl::expected<void, ErrorCode>> BatchGet(
      const std::vector<std::string>& object_keys,
      std::unordered_map<std::string, std::vector<Slice>>& slices);

  // —— 元数据查询（控制面，不搬数据；返回副本描述符=各副本所在段/文件）——
  tl::expected<std::vector<Replica::Descriptor>, ErrorCode> Query(
      const std::string& object_key);
  tl::expected<std::unordered_map<std::string, std::vector<Replica::Descriptor>>,
               ErrorCode> QueryByRegex(const std::string& str);
  std::vector<tl::expected<std::vector<Replica::Descriptor>, ErrorCode>>
      BatchQuery(const std::vector<std::string>& object_keys);

  // —— 用「已查得的副本清单」直接传输（先 Query 后 Get，省一次元数据往返）——
  tl::expected<void, ErrorCode> Get(
      const std::string& object_key,
      const std::vector<Replica::Descriptor>& replica_list,
      std::vector<Slice>& slices);

  // —— 写路径（带副本配置）——
  tl::expected<void, ErrorCode> Put(const ObjectKey& key,
                                    std::vector<Slice>& slices,
                                    const ReplicateConfig& config);
  std::vector<tl::expected<void, ErrorCode>> BatchPut(
      const std::vector<ObjectKey>& keys,
      std::vector<std::vector<Slice>>& batched_slices,
      const ReplicateConfig& config);

  // —— 删除 ——
  tl::expected<void, ErrorCode> Remove(const ObjectKey& key);
  tl::expected<long, ErrorCode>  RemoveByRegex(const ObjectKey& str);
  tl::expected<long, ErrorCode>  RemoveAll();

  // —— 段挂载（向集群贡献本地内存作为内存层容量）——
  tl::expected<void, ErrorCode> MountSegment(const void* buffer, size_t size);
  tl::expected<void, ErrorCode> UnmountSegment(const void* buffer, size_t size);

  // —— 内存注册（透传到 TransferEngine，零拷贝前提；location 如 "cpu:0"/"cuda:0"）——
  tl::expected<void, ErrorCode> RegisterLocalMemory(
      void* addr, size_t length, const std::string& location,
      bool remote_accessible = true, bool update_metadata = true);
  tl::expected<void, ErrorCode> unregisterLocalMemory(
      void* addr, bool update_metadata = true);

  // —— 存在性 ——
  tl::expected<bool, ErrorCode> IsExist(const std::string& key);
  std::vector<tl::expected<bool, ErrorCode>> BatchIsExist(
      const std::vector<std::string>& keys);

  // —— 指标 ——
  tl::expected<std::string, ErrorCode> GetSummaryMetrics();   // 人读
  tl::expected<std::string, ErrorCode> SerializeMetrics();    // Prometheus 风格
};
}  // namespace mooncake
```

**接口要点（对本项目设计的影响）**：

1. **`Query` / `Replica::Descriptor` 是跨节点索引的天然数据源**。`Query(key)` 返回该对象**全部副本**
   的描述符，每个 `MemoryDescriptor` 内含 `AllocatedBuffer::Descriptor{segment_name_, size_,
   buffer_address_, status_}`，`segment_name_` 即持有副本的节点/段名。本项目
   `IObjectStore::Locate(BlockKey) -> std::vector<NodeId>`（设计文档）正是对 `Query` +
   `get_segment_names()` 的封装，用来回填 `Scheduler` 的 `CrossNodeIndex`（需求 4.2）。
2. **读写均以 `Slice{ptr,size}` 向量为单位**，配合 `RegisterLocalMemory` 实现端到端零拷贝——
   与本项目 `Data_Path` 的零拷贝缓冲注册（需求 3.3）语义一致。
3. **`ReplicateConfig.preferred_segment` 暴露放置偏好**，使上层可把某对象引导到特定节点/段——
   这是本项目 `Scheduler::ChoosePlacementNode`（负载均衡，需求 4.4）与 prefix 复用放置
   （需求 4.1）可借力的扩展点。
4. **统一错误模型 `ErrorCode`**：`OBJECT_NOT_FOUND=-704` 对应缓存未命中（需求 2.5），
   `OBJECT_HAS_LEASE=-706`/`REPLICA_IS_NOT_READY=-703`/`TRANSFER_FAIL=-800` 等用于本项目错误处理映射
   （详见 §8.3）。

### 8.2 控制面 `mooncake::MasterService`（集群元数据 / 放置 / 淘汰）

来源：`mooncake-store/include/master_service.h`。这是**多层缓存行为与跨节点放置/淘汰的控制中枢**。

```cpp
class MasterService {
 public:
  // —— 段生命周期（节点向集群注册/注销内存层容量）——
  tl::expected<void, ErrorCode> MountSegment(const Segment&, const UUID& client_id);
  tl::expected<void, ErrorCode> ReMountSegment(const std::vector<Segment>&, const UUID&);
  tl::expected<void, ErrorCode> UnmountSegment(const UUID& segment_id, const UUID& client_id);

  // —— 集群可观测性（调度输入：容量与已用量）——
  tl::expected<std::vector<std::string>, ErrorCode> GetAllKeys();
  tl::expected<std::vector<std::string>, ErrorCode> GetAllSegments();
  tl::expected<std::pair<size_t,size_t>, ErrorCode>  QuerySegments(const std::string& segment);
      //                ^capacity      ^used —— 注释明言「Conductor 应据此调度新请求」

  // —— 键→副本映射（跨节点索引来源）——
  tl::expected<std::vector<Replica::Descriptor>, ErrorCode> GetReplicaList(std::string_view key);
  tl::expected<std::unordered_map<std::string,std::vector<Replica::Descriptor>>, ErrorCode>
      GetReplicaListByRegex(const std::string& regex_pattern);
  std::vector<tl::expected<std::vector<Replica::Descriptor>, ErrorCode>>
      BatchGetReplicaList(const std::vector<std::string>& keys);

  // —— 两阶段写（分配→落数→提交/撤销）——
  tl::expected<std::vector<Replica::Descriptor>, ErrorCode>
      PutStart(const std::string& key, const std::vector<uint64_t>& slice_lengths,
               const ReplicateConfig& config);
  tl::expected<void, ErrorCode> PutEnd(const std::string& key, ReplicaType replica_type);
  tl::expected<void, ErrorCode> PutRevoke(const std::string& key, ReplicaType replica_type);

  // —— 存在性 / 删除 / 心跳 ——
  tl::expected<bool, ErrorCode> ExistKey(const std::string& key);
  tl::expected<void, ErrorCode> Remove(const std::string& key);
  tl::expected<PingResponse, ErrorCode> Ping(const UUID& client_id);  // 心跳 → 失联检测

 private:
  // 近似 LRU 的两趟淘汰：第一趟只淘汰无 soft-pin 的对象，第二趟在需要满足淘汰下界时才动 soft-pin
  void BatchEvict(double evict_ratio_target, double evict_ratio_lowerbound);

  // 淘汰水位与租约（多层缓存关键参数）
  std::atomic<bool> need_eviction_{false};       // 空间不足时置位，触发淘汰
  const double eviction_ratio_;                  // 每次淘汰比例（默认 0.05）
  const double eviction_high_watermark_ratio_;   // 高水位触发阈值（默认 0.95）
  const uint64_t default_kv_lease_ttl_;          // KV 租约 TTL（默认 5000ms）
  const uint64_t default_kv_soft_pin_ttl_;       // soft-pin TTL（默认 30min）
  const bool     allow_evict_soft_pinned_objects_;
};
```

**与本项目设计的对应关系**：

- **`QuerySegments → (capacity, used)`** 与 **`Ping`（失联检测）** 直接喂给本项目
  `Scheduler` 的 `ClusterState`（`NodeState.occupancy_ratio`、`reachable`），支撑高水位负载均衡
  （需求 4.4）与故障切换（需求 4.5）。
- **`eviction_high_watermark_ratio_`（默认 0.95）+ `BatchEvict` 近似 LRU + 两趟 soft-pin 策略**
  是本项目 `Eviction_Policy`（设计文档：分段 LRU + 频率加权、高水位触发、降级到下一层或移除）的
  上游对标；本项目在更细的 HBM→DRAM→NVMe 三层语义上扩展它（详见 §9）。
- **`PutStart/PutEnd/PutRevoke` 两阶段写** 对应 `ReplicaStatus`
  `INITIALIZED→PROCESSING→COMPLETE`（见 §9.1），与本项目 `TieredStorageManager::Write` 的
  「先占位/分配、再落数、再提交」一致，保证「present 块解析到唯一权威层」（需求 2.1）。

### 8.3 错误/状态模型 `ErrorCode`（需求 1.2，衔接 2.5 / 3.6）

来源：`mooncake-store/include/types.h`。与本项目错误处理映射的关键码：

| `ErrorCode` | 数值 | 含义 | 本项目映射 |
|---|---|---|---|
| `OK` | 0 | 成功 | `Status::Ok` |
| `OBJECT_NOT_FOUND` | -704 | 对象不存在 | `LookupResult.hit=false` 缓存未命中（需求 2.5） |
| `REPLICA_IS_NOT_READY` | -703 | 副本未就绪 | 重试 / 视为暂时未命中 |
| `OBJECT_ALREADY_EXISTS` | -705 | 对象已存在 | 写幂等去重 |
| `OBJECT_HAS_LEASE` | -706 | 对象持有租约（不可淘汰） | 淘汰跳过（保护热块） |
| `NO_AVAILABLE_HANDLE` | -200 | 无可用空间分配 | 触发本项目跨层降级/淘汰（需求 2.4） |
| `BUFFER_OVERFLOW` | -10 | 缓冲空间不足 | 背压 / 分片 |
| `TRANSFER_FAIL` | -800 | 传输失败 | 封装进 `TransferError{failing_key,…}`（需求 3.6） |
| `SEGMENT_NOT_FOUND` | -101 | 无可用段 | 放置失败 → 重路由（需求 4.4） |
| `INVALID_REPLICA` / `INVALID_WRITE` / `INVALID_READ` | -702/-700/-701 | 读写/副本非法 | 诊断 + 失败键上报 |
| `RPC_FAIL` | -900 | Master RPC 失败 | 节点失联 → 故障切换（需求 4.5） |

本项目 `store_adapter` 在边界处把 `tl::expected<T,ErrorCode>` 归一化为项目自有的
`Result<T>` / `Status`，并在数据面失败时把上游 `ErrorCode` 连同**失败块键 `BlockKey`** 封装进
`TransferError{failing_key, path, reason}`（需求 3.6）。

### 8.4 Python 绑定面（`PyClient` / `MooncakeDistributedStore`）

来源：`mooncake-store/include/pybind_client.h` 与 `mooncake-integration/store/store_py.cpp`。

上游通过 pybind11 暴露 `MooncakeDistributedStore` 类，这是 **vLLM 的 Mooncake KV connector 实际
对接的 Python 接口面**（需求 6.1 的上游侧锚点）。导出的关键方法（均以 `uintptr_t buffer_ptr`
传裸缓冲地址，配合 `register_buffer` 实现零拷贝）：

```text
MooncakeDistributedStore:
  setup(local_hostname, metadata_server, global_segment_size, local_buffer_size,
        protocol, rdma_devices, master_server_addr)
  init_all(protocol, device_name, mount_segment_size)
  register_buffer(buffer_ptr, size) / unregister_buffer(buffer_ptr)   # 零拷贝注册
  put(key, value, config) / put_parts(key, *parts, config)
  put_from(key, buffer_ptr, size, config)                            # 从已注册缓冲零拷贝写
  put_from_with_metadata(key, buffer_ptr, metadata_buffer_ptr, ...)
  batch_put_from(keys, buffer_ptrs, sizes, config)
  get(key) / get_batch(keys)
  get_into(key, buffer_ptr, size)                                    # 零拷贝读入已注册缓冲
  batch_get_into(keys, buffer_ptrs, sizes)
  get_buffer(key) -> BufferHandle / batch_get_buffer(keys)
  get_tensor(key) / put_tensor(key, tensor)                          # PyTorch 张量直存取
  is_exist(key) / batch_is_exist(keys)                               # 1/0/-1
  get_size(key) / remove(key) / remove_by_regex(pattern) / remove_all() / close()
```

- `PyClient`（`pybind_client.h`）内部持有 `std::shared_ptr<mooncake::Client>` 与
  `ClientBufferAllocator`，并用 `ResourceTracker`（信号/退出钩子单例）保证异常终止时回收资源——
  与本项目 RAII 资源纪律（需求 7.3）理念一致。
- `to_py_ret` 把 `tl::expected<T,ErrorCode>` 统一转成 `int64_t` 返回码（0 成功，负值为 `ErrorCode`）。
- **本项目 `vLLM_Adapter`（`python/vllm_adapter/connector.py`）经此绑定对接，不修改上游源码**
  （需求 6.2）：`ProjectKVConnector.store_kv → put_from/batch_put_from`、
  `load_kv → get_into/batch_get_into`、`supports → setup/init_all 的 protocol 能力探测`。

---

## 9. 键值对象模型与多层缓存行为（需求 1.2）

### 9.1 对象 / 副本 / 段 三级模型

来源：`types.h` + `replica.h` + `allocator.h`。

```text
ObjectKey (std::string)                ← 对象的全局唯一键
   └─ ReplicaList = unordered_map<uint32_t, Replica>   ← 一个对象的若干副本（按副本号索引）
        └─ Replica { variant<MemoryReplicaData, DiskReplicaData>, ReplicaStatus }
             ├─ MemoryReplicaData { vector<unique_ptr<AllocatedBuffer>> }   ← 内存层（DRAM/VRAM）
             │      └─ AllocatedBuffer { allocator, segment_name, buffer_ptr, size, offset_handle(RAII) }
             └─ DiskReplicaData    { file_path, object_size }               ← 磁盘层（NVMe/SSD/3FS）
```

关键类型（真实定义）：

- `using ObjectKey = std::string;`、`using Version = uint64_t;`、`using SegmentId = int64_t;`
  （`types.h`）。
- `struct Slice { void* ptr; size_t size; };`，切片大小受 CacheLib Slab 约束：
  `kMinSliceSize = Slab::kMinAllocSize`、`kMaxSliceSize = Slab::kSize - 16`（`types.h`）。
- `struct Segment { UUID id; std::string name; uintptr_t base; size_t size; };`（`types.h`）——
  `name` 通常即节点 `ip:port`，是把副本定位到**节点**的关键。
- `enum class ReplicaType { MEMORY, DISK };`、
  `enum class ReplicaStatus { UNDEFINED, INITIALIZED, PROCESSING, COMPLETE, REMOVED, FAILED };`
  （`replica.h`）。
- `struct ReplicateConfig { size_t replica_num{1}; bool with_soft_pin{false};
  std::string preferred_segment{}; };`（`replica.h`）。
- `class AllocatedBuffer`：内存副本的 RAII 缓冲句柄，`get_descriptor()` 产出可序列化的
  `Descriptor{segment_name_, size_, buffer_address_, status_}`，析构时经 `offset_handle_` 自动归还
  空间（`allocator.h`）——印证上游内存层的资源守恒纪律。

### 9.2 多层缓存行为（HBM/DRAM 内存层 ↔ NVMe/SSD 磁盘层）

Store_Layer 的「多层」在锁定 commit 中体现为**两类副本 + 可插拔分配/淘汰策略 + 持久化后端**：

1. **内存层（`ReplicaType::MEMORY`）**：由各节点 `MountSegment` 贡献的 DRAM/VRAM 段构成，经
   `BufferAllocatorBase` 的两种实现分配：
   - `CachelibBufferAllocator`（CacheLib slab 分配，`getLargestFreeRegion()` 返回未知近似值）；
   - `OffsetBufferAllocator`（offset 分配器，精确返回最大空闲区）。
   `BufferAllocatorType { CACHELIB=0, OFFSET=1 }`（`types.h`）。
2. **磁盘层（`ReplicaType::DISK`）**：由 `StorageBackend`（`storage_backend.h`）提供持久化，
   支持本地文件系统与 **3FS**（`USE_3FS` 编译期开关，`USRBIOResourceManager` + `Hf3fs` cuFile/GDS
   路径），接口为 `StoreObject(path, slices/str/span)` / `LoadObject(path, slices, length)` /
   `RemoveFile` / `RemoveByRegex` / `RemoveAll`。这对应本项目的 **NVMe 层**。
3. **放置策略 `AllocationStrategy`（`allocation_strategy.h`）**：`RandomAllocationStrategy` 采用
   **best-effort + 跨段冗余 + `preferred_segment` 优先** 语义：每个 slice 的多个副本被放到不同段以
   保证冗余；空间不足时尽量分配（至少 1 个），全部失败才返回 `NO_AVAILABLE_HANDLE`。
4. **淘汰策略 `EvictionStrategy`（`eviction_strategy.h`）**：抽象接口
   `AddKey/UpdateKey/RemoveKey/EvictKey/GetSize/CleanUp`，内置 `LRUEvictionStrategy`（访问即移到表头、
   淘汰表尾）与 `FIFOEvictionStrategy`。`MasterService::BatchEvict` 在高水位
   （`eviction_high_watermark_ratio_`，默认 0.95）时按近似 LRU + 两趟 soft-pin 触发淘汰。
5. **租约 / soft-pin 保护热块**：`default_kv_lease_ttl_`（默认 5000ms）期间对象持有租约
   （`OBJECT_HAS_LEASE` 拒绝淘汰）；`with_soft_pin` + `default_kv_soft_pin_ttl_`（默认 30min）进一步
   保护高价值对象，仅在第二趟且必要时才淘汰。

> **多层缓存行为小结**：上游 Store_Layer 已提供「内存副本 ↔ 磁盘副本」两层 + 可插拔放置/淘汰 + 租约
> 保护的完整骨架，但其层级语义是**两层（DRAM/VRAM 内存 vs NVMe/SSD/3FS 磁盘）**且淘汰为单层近似
> LRU。本项目 `Tiered_Storage_Manager` 在此之上细化为 **HBM→DRAM→NVMe 三层**、补充**跨层降级链**
> （victim 逐层下沉、最慢层无下层则移除）、**频率加权的分段 LRU**、以及**基于访问信号的预取**
> （需求 2.1–2.7）。两者是「上游提供存储基元、本项目提供三层智能编排」的分层互补关系。

### 9.3 KVCache_Block 键 → Store 键映射（需求 1.2 / 1.4）

本项目数据模型（设计文档）：

```cpp
struct BlockKey { uint64_t hash_id; uint32_t layer; uint16_t version = 1; /* operator== */ };
```

上游 `ObjectKey = std::string`。两者通过 `store_adapter` 做**确定性、可逆的字符串编码**桥接，
约定如下规范键格式：

```text
ObjectKey := "kvb/" <hash_id:16hex> "/" <layer:dec> "/v" <version:dec>
  示例：BlockKey{hash_id=0xA1B2…, layer=7, version=1}  ⇄  "kvb/000000000000a1b2/7/v1"
```

映射规则与不变量：

| `BlockKey` 字段 | 编码进 `ObjectKey` 的方式 | 说明 |
|---|---|---|
| `hash_id` | 定宽 16 位十六进制（小端规整为大端可读串） | 内容哈希，对应 FAST25 trace 的 `hash_ids`，保证前缀复用可比对（需求 4.1） |
| `layer` | 十进制 | Transformer 层号；同一 token 块各层是独立对象，便于按层并行传输 |
| `version` | `v` 前缀 + 十进制 | 序列化/schema 版本，演进时键空间隔离，避免脏读 |

- **前缀命名空间 `kvb/`** 使本项目对象与同集群其它 Mooncake 使用者隔离，并可用
  `Client::QueryByRegex("kvb/.*")` / `RemoveByRegex` 做范围运维。
- 编码**单射且可逆**：`encode(BlockKey)` 与 `decode(ObjectKey)` 互逆，保证
  `Locate`/`Exists`/`Get`/`Put` 在键层面无歧义（支撑设计文档「BlockKey 结构相等 + 序列化往返」，
  需求 3.5）。该可逆性将由适配器单测（任务 3.4）与序列化往返属性测试（Property 6）守护。
- **值映射**：`KVCacheBlock` 经 `Data_Path::Serialize` 产出 `SerializedBlock`（`[header | shape/dtype
  | K | V]` 定长前缀、带版本），再以一个或多个 `Slice` 形式经 `Client::Put` 写入；读路径
  `Client::Get` 把字节回填到已注册缓冲后 `Deserialize` 还原为等价 `KVCacheBlock`。

---

## 10. 扩展点完整枚举（需求 1.4）

下表枚举本项目各组件将构建于其上的 **Transfer_Engine + Store_Layer 真实扩展点**，并标注挂载方式与
关联需求。这是「分析先行门禁」要求的扩展点清单（需求 1.4）。

### 10.1 Transfer_Engine 扩展点（数据面）

| # | 上游扩展点（真实接口） | 头文件 | 本项目挂载组件 | 关联需求 |
|---|---|---|---|---|
| TE-1 | `registerLocalMemory` / `unregisterLocalMemory`（+ `location` 网卡亲和） | `transfer_engine.h` | `Data_Path` 经 `ITransferBackend::RegisterBuffer/DeregisterBuffer`、`ScopedRegistration`（RAII） | 3.3、7.3 |
| TE-2 | 多后端 `Transport`（rdma/tcp/nvmeof/nvlink）+ `MultiTransport::selectTransport` | `transport.h`、`multi_transport.h` | `Data_Path` 经 `ITransferBackend::SelectPath`，`TransportPath{kRdma,kGpuDirect,kTcp}` 能力探测与回退 | 3.1、3.2、3.7 |
| TE-3 | `allocateBatchID` + `submitTransfer`（异步立即返回）+ `getTransferStatus` | `transfer_engine.h` | `Data_Path` 异步流水线 `SubmitAsync`（future）+ 有界提交队列背压 | 3.4 |
| TE-4 | `Status` 错误码（`kAddressNotRegistered`/`kNotSupportedTransport`/…） | `status.h` | `Data_Path` 故障路径 → `TransferError{failing_key, path, reason}` | 3.6 |
| TE-5 | `submitTransferWithNotify` / `getNotifies`（带外通知） | `transfer_engine.h` | `Scheduler` 跨节点完成事件协同 | 4.x |
| TE-6 | `Topology` 拓扑矩阵 + 多网卡故障切换 | `topology.h` | `Scheduler` 节点级故障切换/负载均衡的传输层基础 | 4.4、4.5 |

### 10.2 Store_Layer 扩展点（控制面 + 对象级 K/V）

| # | 上游扩展点（真实接口） | 头文件 | 本项目挂载组件 | 关联需求 |
|---|---|---|---|---|
| ST-1 | `Client::Put / BatchPut`（`ReplicateConfig`，含 `preferred_segment`） | `client.h` | `Tiered_Storage_Manager::Write` 经 `IObjectStore::Put`（落数到内存/磁盘副本） | 2.1、2.2 |
| ST-2 | `Client::Get / BatchGet`（零拷贝进 `Slice`） | `client.h` | `Tiered_Storage_Manager::Read` 经 `IObjectStore::Get`；命中/未命中判定 | 2.3、2.5 |
| ST-3 | `Client::IsExist / BatchIsExist` | `client.h` | `IObjectStore::Exists`；缓存未命中报告 | 2.5 |
| ST-4 | `Client::Query / QueryByRegex` + `Replica::Descriptor`（`segment_name`） | `client.h`、`replica.h` | `IObjectStore::Locate -> vector<NodeId>`；回填 `Scheduler::CrossNodeIndex` | 4.2、1.4 |
| ST-5 | `MasterService::QuerySegments → (capacity, used)` + `Ping`（失联检测） | `master_service.h` | `Scheduler` 的 `ClusterState`/`NodeState`（高水位、reachable） | 4.4、4.5 |
| ST-6 | `MountSegment` / `UnmountSegment`（贡献内存层容量） | `client.h`、`master_service.h` | `Tiered_Storage_Manager` 内存层（HBM/DRAM）容量编排 | 2.1、2.7 |
| ST-7 | `AllocationStrategy`（best-effort + `preferred_segment` 偏好放置） | `allocation_strategy.h` | `Scheduler::ChoosePlacementNode` 放置决策对标/扩展 | 4.4 |
| ST-8 | `EvictionStrategy`（LRU/FIFO）+ `MasterService::BatchEvict` + 高水位/租约 | `eviction_strategy.h`、`master_service.h` | `Eviction_Policy`（分段 LRU + 频率加权 + 三层降级链） | 2.4 |
| ST-9 | `StorageBackend`（本地 FS / 3FS-GDS 持久化） | `storage_backend.h` | `Tiered_Storage_Manager` 的 **NVMe 层** 落地 | 2.1、2.7 |
| ST-10 | `AllocatedBuffer`（RAII 句柄）/ `BufferAllocatorBase`（capacity/size/largestFree） | `allocator.h` | 内存层占用统计与资源守恒（喂 `TierOccupancy`） | 2.4、7.3 |

### 10.3 Mooncake KV connector 接口面（vLLM 集成扩展点，需求 1.4 / 6.1）

| # | 上游扩展点（真实接口） | 来源 | 本项目挂载组件 | 关联需求 |
|---|---|---|---|---|
| KV-1 | `MooncakeDistributedStore.setup / init_all`（protocol/段尺寸/master 地址） | `store_py.cpp` | `ProjectKVConnector.__init__` + `supports()` 能力探测 | 6.1、6.3 |
| KV-2 | `register_buffer / unregister_buffer`（零拷贝注册） | `store_py.cpp`、`pybind_client.h` | `vLLM_Adapter` 把 vLLM KV 张量缓冲注册到引擎 | 6.1、3.3 |
| KV-3 | `put_from / batch_put_from / put_tensor`（写） | `store_py.cpp` | `ProjectKVConnector.store_kv`（经 Scheduler 路由 + Data_Path 传输） | 6.1 |
| KV-4 | `get_into / batch_get_into / get_buffer / get_tensor`（读） | `store_py.cpp` | `ProjectKVConnector.load_kv` | 6.1 |
| KV-5 | `is_exist / batch_is_exist / get_size`（命中探测） | `store_py.cpp` | connector 命中/复用率统计输入（喂 Benchmark） | 5.4、6.1 |
| KV-6 | `ResourceTracker`（信号/退出钩子，异常清理） | `pybind_client.h` | 绑定层资源回收对标（不改上游源码） | 6.2、7.3 |

---

## 11. 临时（provisional）组件接口签名对账与门禁签收（需求 1.5）

设计文档（`design.md`）声明：在本分析工件 pin 定上游 commit 并确认公开面之前，组件接口签名为
**provisional（临时）**，需对账后方可锁定。以下逐项对账，确认设计文档中的项目自有接口签名与上游
真实接口**语义可实现、无冲突**。

### 11.1 `ITransferBackend`（Transfer_Engine 适配）对账

| 设计文档签名 | 上游真实支撑 | 对账结论 |
|---|---|---|
| `RegisterBuffer(addr,len,on_gpu) -> Result<BufferHandle>` | `TransferEngine::registerLocalMemory(addr,len,location,remote_accessible)` | ✅ 可实现：`on_gpu` → `location="cuda:N"`；`reg_id` 由适配器内部映射 |
| `DeregisterBuffer(BufferHandle) -> Status` | `unregisterLocalMemory(addr)` | ✅ 一一对应；RAII 配对（需求 7.3） |
| `SelectPath(req) -> TransportPath` | `MultiTransport::selectTransport` + `getTransport(proto)` 探测 | ✅ 可实现：探测 `"rdma"/"tcp"/"nvmeof"` 安装情况后映射 |
| `SubmitAsync(req) -> std::future<Result<TransferReceipt>>` | `allocateBatchID`+`submitTransfer`（异步返回）+`getTransferStatus` 轮询 | ✅ 可实现：future 由适配器后台轮询 `COMPLETED/FAILED` 兑现 |

### 11.2 `IObjectStore`（Store_Layer 适配）对账

| 设计文档签名 | 上游真实支撑 | 对账结论 |
|---|---|---|
| `Put(BlockKey, SerializedBlock) -> Status` | `Client::Put(ObjectKey, vector<Slice>&, ReplicateConfig)` | ✅ 可实现：`encode(BlockKey)→ObjectKey`，`SerializedBlock.bytes` 切为 `Slice` 向量 |
| `Get(BlockKey) -> Result<SerializedBlock>` | `Client::Get(ObjectKey, vector<Slice>&)`（+ 先 `Query`） | ✅ 可实现：读入已注册缓冲后组装 `SerializedBlock` |
| `Exists(BlockKey) -> bool` | `Client::IsExist(ObjectKey)` | ✅ 直接对应（`OBJECT_NOT_FOUND` → false） |
| `Locate(BlockKey) -> std::vector<NodeId>` | `Client::Query` → `Replica::Descriptor` → `segment_name`（`Segment.name = ip:port`） | ✅ 可实现：由副本描述符的 `segment_name` 解析出持有节点集合 |

### 11.3 组件级签名对账

| 组件接口（design.md） | 上游支撑扩展点 | 对账结论 |
|---|---|---|
| `TieredStorageManager::{Write,Read,Locate,EnforceCapacity,Prefetch}` | ST-1/2/6/8/9/10 + TE-1/3 | ✅ 三层语义为本项目在上游两层副本之上的**扩展**，无冲突 |
| `DataPath::{Serialize,Deserialize,TransferAsync}` | TE-1/2/3/4 + ST-1/2 | ✅ 序列化为项目自有；传输全部经 `ITransferBackend` |
| `CrossNodeIndex::{Register,Unregister,Lookup}` | ST-4（`Query`/`Replica::Descriptor`）+ ST-5（`Ping`） | ✅ 索引可由 `Query` 回填、由 `Ping` 失联事件维护 |
| `Scheduler::{Route,AllocateFairShare,ChoosePlacementNode,ResolveOnFailure}` | ST-5/7 + TE-6 | ✅ 公平分配/路由为项目自有控制面逻辑，放置/故障切换借力上游可观测性 |
| `ProjectKVConnector::{store_kv,load_kv,supports}` | KV-1..6 | ✅ 经 `MooncakeDistributedStore` 绑定实现，不改上游源码（需求 6.2） |

### 11.4 发现的差异与处理（不修改上游源码，需求 6.2）

1. **层级粒度差异**：上游为「内存/磁盘」两层，本项目需 HBM/DRAM/NVMe 三层。
   *处理*：内存层在本项目侧再按 `location`（`cuda:*` = HBM、`cpu:*` = DRAM）二分，磁盘层映射 NVMe；
   三层编排逻辑落在本项目 `Tiered_Storage_Manager`，**不改上游**。
2. **淘汰语义差异**：上游 `BatchEvict` 为单层近似 LRU + soft-pin；本项目需频率加权分段 LRU + 跨层
   降级链。*处理*：本项目在适配器之上实现自有 `Eviction_Policy`，把上游淘汰作为最慢层兜底，
   二者分层叠加。
3. **键模型差异**：上游键为不透明 `std::string`；本项目键为结构化 `BlockKey`。
   *处理*：`store_adapter` 提供单射可逆编码（§9.3），由单测/属性测试守护。
4. **错误模型差异**：上游 `tl::expected<T,ErrorCode>` vs 本项目 `Result<T>/Status`。
   *处理*：适配器边界统一转换（§8.3 映射表）。

> 以上差异均可在**适配器层**消化，无需修改 `third_party/mooncake/` 任何源码，满足需求 6.2。

### 11.5 门禁签收（需求 1.5）

- [x] **需求 1.1**：Transfer_Engine 公开接口 / 传输协议 / register→submit→complete 数据流——已在
  第一部分（§1–§5）成文，基于真实头文件。
- [x] **需求 1.2**：Store_Layer 公开接口（§8）、键值对象模型（§9.1、§9.3）、多层缓存行为（§9.2）——
  已成文，基于 `client.h`/`types.h`/`replica.h`/`allocator.h`/`master_service.h` 等真实头文件。
- [x] **需求 1.3**：锁定上游 commit `356d99fb28746d274241b6792c2f7c2fe17e3b29`（v0.3.6.post1）已记录，
  与 `mooncake.lock` 一致（§0），并经 `git -C third_party/mooncake rev-parse HEAD` 核对。
- [x] **需求 1.4**：Transfer_Engine（§10.1，TE-1..6）、Store_Layer（§10.2，ST-1..10）、Mooncake KV
  connector（§10.3，KV-1..6）扩展点已完整枚举。
- [x] **需求 1.5**：设计文档 provisional 组件接口签名已对账（§11.1–§11.3），差异均可在适配器层消化
  且不改上游源码（§11.4）。

**门禁结论（GATE: PASS）**：分析先行门禁**已签收**。Tiered_Storage_Manager、Data_Path、Scheduler
的设计据此从「provisional」转为「已锁定（locked）」，下游任务（任务 3 适配器层及之后）可在本分析
工件确认的扩展点之上推进。后续若上游 pin 定 commit 变更，须重跑本分析并重新签收门禁。
