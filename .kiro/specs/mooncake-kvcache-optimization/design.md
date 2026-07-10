# Design Document

## Overview

This document describes the design of a KVCache optimization Project layered on top of the
upstream Mooncake project (kvcache-ai/Mooncake). The Project is delivered as a **standalone Git
repository** that consumes Mooncake as a **submodule/library dependency** through well-defined
**adapter boundaries**, and is packaged for "file + link" contest submission rather than as an
in-tree upstream change.

The Project delivers four integrated capabilities plus supporting infrastructure:

1. **Tiered_Storage_Manager** — HBM→DRAM→NVMe placement with prefetch and eviction.
2. **Data_Path** — zero-copy buffer registration, RDMA/GPUDirect via the Mooncake Transfer Engine,
   an asynchronous I/O pipeline, KV serialization, and a TCP fallback.
3. **Scheduler** — cross-node prefix-aware reuse index, multi-tenant fair-share allocation, and
   failover/load balancing.
4. **Benchmark_Framework** — FAST'25 trace replay against a vLLM PagedAttention baseline, with
   throughput/TTFT/latency-percentile, cache-hit/reuse-rate reporting and reproducibility.

Plus the **vLLM_Adapter** (integration through the Mooncake KV connector) and the cross-cutting
coding-standard, memory-safety, and thread-safety requirements.

Core storage and transfer paths are implemented in **C++**; benchmarking, orchestration, and vLLM
integration are implemented in **Python**.

### Analysis-First Constraint (Requirement 1)

> **Design gate.** Requirement 1 mandates that a documented analysis of the upstream Mooncake
> Transfer Engine and Mooncake Store (interfaces, transport protocols, data-movement flow,
> key/value object model, multi-tier behavior, extension points, and the **pinned commit hash**)
> be completed **before** the design of the Tiered_Storage_Manager, Data_Path, and Scheduler is
> finalized.

This document is therefore structured so that the **Upstream Mooncake Analysis** section
(Section "Upstream Mooncake Dependency Analysis") is authored and reviewed first, and serves as the
input to the component designs that follow. The analysis is maintained as a living artifact at
`docs/analysis/upstream-mooncake-analysis.md` and is referenced by, not duplicated in, the component
designs. All Project↔Mooncake interfaces described below assume the extension points enumerated in
that analysis. Until that artifact pins a concrete upstream commit and confirms the public surface,
the interface signatures here are treated as **provisional** and finalized only after analysis sign-off.

## Architecture

### System Context

```
                        ┌──────────────────────────────────────────────┐
                        │                  vLLM Engine                   │
                        │        (PagedAttention, KV connector API)      │
                        └───────────────────────┬────────────────────────┘
                                                 │ Mooncake KV connector
                                                 ▼
        ┌────────────────────────────────────────────────────────────────────────┐
        │                          PROJECT (this repo)                             │
        │                                                                          │
        │   ┌──────────────┐   ┌───────────────────────┐   ┌───────────────────┐  │
        │   │ vLLM_Adapter │──▶│      Scheduler        │──▶│ Tiered_Storage_   │  │
        │   │  (Python)    │   │  (prefix index, fair  │   │ Manager (C++)     │  │
        │   └──────────────┘   │  share, failover/LB)  │   │ HBM▸DRAM▸NVMe     │  │
        │          │           └───────────┬───────────┘   │ prefetch+evict    │  │
        │          │                       │               └─────────┬─────────┘  │
        │          │                       ▼                         ▼            │
        │          │              ┌────────────────────────────────────────────┐ │
        │          └─────────────▶│                Data_Path (C++)              │ │
        │                         │ zero-copy reg • async I/O • serialization   │ │
        │                         │ RDMA/GPUDirect • TCP fallback               │ │
        │                         └───────────────────┬────────────────────────┘ │
        │                                              │ Adapter boundary         │
        │   ┌───────────────────┐                      │                          │
        │   │ Benchmark_        │                      │                          │
        │   │ Framework (Py)    │                      │                          │
        │   │ FAST25 replay     │                      │                          │
        │   └───────────────────┘                      │                          │
        └──────────────────────────────────────────────┼──────────────────────────┘
                                                        ▼
        ┌────────────────────────────────────────────────────────────────────────┐
        │                    UPSTREAM MOONCAKE (submodule)                         │
        │   ┌──────────────────────────┐      ┌────────────────────────────────┐  │
        │   │     Store_Layer          │◀────▶│        Transfer_Engine         │  │
        │   │ (distributed K/V cache)  │      │ TCP/RDMA/GPUDirect/NVMe-oF,    │  │
        │   │                          │      │ multi-NIC, topology routing    │  │
        │   └──────────────────────────┘      └────────────────────────────────┘  │
        └────────────────────────────────────────────────────────────────────────┘
```

### Repository and Dependency Layout

The Project does not modify upstream Mooncake in place (Requirement 6.2). Mooncake is referenced as
a Git submodule pinned to a recorded commit, and all interaction goes through a thin **adapter layer**
that isolates the rest of the Project from upstream API churn.

```
repo-root/
├── third_party/mooncake/              # Git submodule, pinned commit (read-only to us)
├── mooncake.lock                      # pinned commit hash + version (Req 1.3, 6.3)
├── src/
│   ├── adapter/                       # C++ adapter over Transfer_Engine + Store_Layer
│   │   ├── transfer_engine_adapter.*  # zero-copy reg, async submit, RDMA/TCP path
│   │   └── store_adapter.*            # K/V object model bridge
│   ├── storage/                       # Tiered_Storage_Manager, Prefetch_Engine, Eviction_Policy
│   ├── datapath/                      # serialization, async pipeline, transfer orchestration
│   └── scheduler/                     # cross-node index, fair-share, failover/LB
├── python/
│   ├── vllm_adapter/                  # Mooncake KV connector integration
│   └── bench/                         # Benchmark_Framework (FAST25 replay, metrics)
├── docs/analysis/                     # Requirement-1 upstream analysis artifact
└── tests/                             # unit + property + integration + sanitizer jobs
```

### Layering and Boundaries

- **Adapter boundary (Project↔Mooncake).** Only `src/adapter/*` includes upstream Mooncake headers
  or links its libraries. Every other Project module depends on Project-owned interfaces
  (`ITransferBackend`, `IObjectStore`) that the adapter implements. This keeps the upstream coupling
  surface small and auditable, and lets the build fail with a clear message when the pinned version
  is unavailable (Requirement 6.5).
- **Control plane vs. data plane.** The Scheduler is a control-plane component (routing, indexing,
  fairness) and never copies block payloads. The Data_Path is the data plane and performs all bulk
  movement through the Transfer_Engine adapter.
- **Language boundary.** Python orchestration (vLLM_Adapter, Benchmark_Framework) calls into the C++
  core through a pybind11 binding exposed by the adapter and storage layers.

## Upstream Mooncake Dependency Analysis (Requirement 1 — authored first)

This section summarizes the analysis artifact that **must be finalized before** the component
designs below are locked. It is grounded in the upstream Mooncake architecture (Transfer Engine +
Mooncake Store).

- **Transfer_Engine surface (Req 1.1).** A unified batched data-movement API across heterogeneous
  transports (TCP, RDMA/RoCE, GPUDirect, NVMe-oF, NVLink, and vendor transports), with multi-NIC
  bandwidth aggregation, topology-aware path selection, and automatic failover on transient network
  errors. The data-movement flow registers memory regions, submits batched transfer descriptors
  (source/target addresses + lengths), and signals completion asynchronously.
- **Store_Layer surface (Req 1.2).** A distributed key/value object store built on the Transfer
  Engine, providing put/get of large objects with striping, parallel I/O, end-to-end zero-copy, and
  a multi-tier (DRAM/SSD-NVMe) cache hierarchy. Objects are addressed by key; the Project maps
  KVCache_Block keys onto Store keys.
- **Pinned baseline (Req 1.3).** The exact upstream commit hash and release version are recorded in
  `mooncake.lock` and echoed into every benchmark report (Req 5.5).
- **Extension points (Req 1.4).** (a) Memory-region registration / zero-copy buffer handles for the
  Data_Path; (b) batched async transfer submission + completion callbacks for the async pipeline and
  TCP/RDMA path selection; (c) Store put/get/exists and metadata hooks for the Tiered_Storage_Manager
  and the Scheduler's cross-node index; (d) the Mooncake KV connector interface for the vLLM_Adapter.

> The component interface signatures in the following sections are **provisional** and are reconciled
> against this analysis (and the pinned commit's real headers) before the design is declared final.
> This satisfies Requirement 1.5's ordering gate.

## Components and Interfaces

Interfaces are shown in C++ for core paths and Python for orchestration. Types prefixed with the
adapter namespace are Project-owned abstractions implemented over upstream Mooncake.

### Project↔Mooncake Adapter Interfaces

```cpp
// src/adapter/transfer_engine_adapter.h
namespace project::adapter {

enum class TransportPath { kRdma, kGpuDirect, kTcp };

struct BufferHandle {                 // opaque zero-copy registered region
  void*    addr   = nullptr;
  size_t   length = 0;
  uint64_t reg_id = 0;                // Transfer_Engine registration id
  bool     on_gpu = false;
};

struct TransferRequest {
  BlockKey      key;
  BufferHandle  src;
  BufferHandle  dst;
  TransportPath preferred;            // selected from detected capabilities
};

class ITransferBackend {              // implemented over Mooncake Transfer_Engine
 public:
  virtual ~ITransferBackend() = default;

  // Zero-copy: register an existing buffer; payload is never copied through an
  // intermediate buffer (Req 3.3). Caller owns the buffer; registration is tracked.
  virtual Result<BufferHandle> RegisterBuffer(void* addr, size_t len, bool on_gpu) = 0;
  virtual Status               DeregisterBuffer(const BufferHandle&) = 0;       // Req 7.3

  // Path selection: RDMA/GPUDirect when available, else TCP fallback (Req 3.1,3.2,3.7).
  virtual TransportPath SelectPath(const TransferRequest&) const = 0;

  // Async submit: returns immediately with a pending future (Req 3.4).
  virtual std::future<Result<TransferReceipt>> SubmitAsync(TransferRequest) = 0;
};

}  // namespace project::adapter
```

```cpp
// src/adapter/store_adapter.h  — bridges KVCache_Block keys onto Mooncake Store
class IObjectStore {
 public:
  virtual Status              Put(const BlockKey&, const SerializedBlock&) = 0;
  virtual Result<SerializedBlock> Get(const BlockKey&) = 0;
  virtual bool                Exists(const BlockKey&) const = 0;
  virtual std::vector<NodeId> Locate(const BlockKey&) const = 0;   // backs cross-node index
};
```

### Tiered_Storage_Manager (C++)

Manages KVCache_Block placement and movement across HBM, DRAM, and NVMe. Drives the Prefetch_Engine
and Eviction_Policy. Operates correctly when NVMe is not configured (Req 2.7).

```cpp
// src/storage/tiered_storage_manager.h
enum class Tier { kHBM = 0, kDRAM = 1, kNVMe = 2 };   // faster → slower

struct TierConfig {
  std::optional<size_t> hbm_capacity_bytes;
  std::optional<size_t> dram_capacity_bytes;
  std::optional<size_t> nvme_capacity_bytes;          // nullopt => NVMe disabled (Req 2.7)
  double                high_water_ratio = 0.9;        // eviction trigger threshold
};

struct LookupResult {
  bool   hit = false;                                  // false => cache miss (Req 2.5)
  Tier   tier = Tier::kHBM;
  BlockKey key;
};

class TieredStorageManager {
 public:
  // Write: place in target tier if below threshold, else trigger eviction first (Req 2.2, 2.4).
  Status Write(const KVCacheBlock& block, Tier target);

  // Read: returns hit/miss; on hit from a slower tier, records access for promotion (Req 2.3, 2.5).
  Result<KVCacheBlock> Read(const BlockKey& key);

  // Placement invariant: a present block resolves to exactly one authoritative tier (Req 2.1).
  LookupResult Locate(const BlockKey& key) const;

  // Eviction: select demote/remove victims by recency+frequency when at/above threshold (Req 2.4).
  Status EnforceCapacity(Tier tier);

  // Prefetch: promote blocks ahead of predicted reuse from access signals (Req 2.6).
  Status Prefetch(const std::vector<BlockKey>& predicted);
};
```

**Prefetch_Engine.** Consumes access signals (recency, frequency, and prefix-reuse hints surfaced by
the Scheduler) and issues promotions to a faster tier ahead of predicted reuse. Promotions reuse the
Data_Path so payload movement is zero-copy where possible.

**Eviction_Policy.** A documented recency-and-frequency policy (segmented LRU with frequency
weighting). When a tier is at or above `high_water_ratio`, victims are demoted to the next slower
tier; when no slower tier exists (e.g., NVMe disabled, or victim already in the slowest tier), they
are removed. The policy guarantees post-eviction occupancy `<=` configured capacity.

### Data_Path (C++)

Performs KVCache_Block read/write transfers using zero-copy buffer registration, RDMA/GPUDirect via
the Transfer_Engine adapter, an async pipeline, and KV serialization, with a TCP fallback.

```cpp
// src/datapath/data_path.h
class DataPath {
 public:
  // Serialization round-trip: Deserialize(Serialize(b)) == b (Req 3.5).
  static SerializedBlock Serialize(const KVCacheBlock& block);
  static Result<KVCacheBlock> Deserialize(const SerializedBlock& bytes);

  // Async transfer; returns before completion (Req 3.4). Selected path honors
  // RDMA/GPUDirect availability, else TCP (Req 3.1, 3.2, 3.7), zero-copy (Req 3.3).
  std::future<Result<TransferReceipt>> TransferAsync(const TransferRequest& req);

  // On failure, the reported error carries the failing block key (Req 3.6).
  // struct TransferError { BlockKey failing_key; TransportPath path; std::string reason; };
};
```

**Async I/O pipeline.** A bounded submission queue feeds the Transfer_Engine adapter's `SubmitAsync`.
Completions are delivered via futures/callbacks, allowing the caller (Scheduler or Prefetch_Engine)
to continue execution. Backpressure is applied when the queue is full.

**Resource discipline (Req 7.3).** Every `RegisterBuffer` is paired with a `DeregisterBuffer` via an
RAII `ScopedRegistration` guard; allocations on the path are owned by RAII handles so that any
acquire/transfer/release sequence returns net registrations and allocations to zero, including on
error paths.

### Scheduler (C++ control plane)

```cpp
// src/scheduler/scheduler.h
struct RouteDecision {
  NodeId node;
  bool   reuse_prefix = false;       // routed to reuse existing prefix blocks (Req 4.1)
  bool   recompute    = false;       // failover to recomputation (Req 4.5)
};

class CrossNodeIndex {               // maps block key → set of holding nodes (Req 4.2)
 public:
  void Register(const BlockKey&, NodeId);
  void Unregister(const BlockKey&, NodeId);
  std::vector<NodeId> Lookup(const BlockKey&) const;   // exactly current holders
};

class Scheduler {
 public:
  // Prefix-aware routing; when disabled by config, the prefix index is not consulted (Req 4.1, 4.6).
  RouteDecision Route(const Request& req);

  // Fair-share allocation across concurrent tenants per documented policy (Req 4.3).
  Allocation AllocateFairShare(const std::vector<TenantDemand>& demands);

  // Load balancing: steer placement to nodes below high-water when any over high-water (Req 4.4).
  NodeId ChoosePlacementNode(const ClusterState& state);

  // Failover: unreachable holder → alternative reachable holder or recompute (Req 4.5).
  RouteDecision ResolveOnFailure(const BlockKey& key, const ClusterState& state);
};
```

**Fair-share policy.** Weighted max-min fairness over per-tenant cache-capacity demand: no tenant is
granted more than its fair share while another tenant with unmet demand is starved; spare capacity is
redistributed to tenants with residual demand.

**Failover/load balancing.** The Scheduler treats node reachability and high-water occupancy as
inputs. When the preferred holder is unreachable it selects an alternative holder from the cross-node
index; if none is reachable it signals recomputation. New placements avoid nodes at or above their
high-water threshold whenever a below-threshold node exists.

### vLLM_Adapter (Python)

Connects the Project to vLLM through the Mooncake KV connector interface (Req 6.1). It translates
vLLM KV-connector load/store callbacks into Scheduler routing + Data_Path transfers, and exposes the
Project's tiered store as the connector's backing KV pool. It performs no upstream source
modification; it depends on the pinned Mooncake build (Req 6.2, 6.3).

```python
# python/vllm_adapter/connector.py
class ProjectKVConnector:  # implements the Mooncake KV connector contract
    def __init__(self, config: ConnectorConfig, scheduler: Scheduler, store: TieredStore): ...
    def store_kv(self, request_id: str, block_keys: list[BlockKey], tensors) -> None: ...
    def load_kv(self, request_id: str, block_keys: list[BlockKey]) -> LoadResult: ...
    def supports(self) -> Capabilities: ...   # reports RDMA/GPUDirect/NVMe availability
```

### Benchmark_Framework (Python)

```python
# python/bench/framework.py
@dataclass
class RunConfig:
    target: Literal["project", "baseline"]   # baseline = vLLM PagedAttention + prefix cache
    trace_path: str                           # FAST25_Trace (mooncake_trace.jsonl)
    hardware: HardwareProfile
    mooncake_commit: str                      # pinned hash echoed into report (Req 5.5)

@dataclass
class RunReport:
    config: RunConfig
    software_versions: dict[str, str]
    mooncake_commit: str
    throughput_tok_s: float
    ttft_ms: Percentiles                      # p50/p90/p99 (Req 5.3)
    e2e_latency_ms: Percentiles
    cache_hit_rate: float                     # Req 5.4
    reuse_rate: float                         # Req 5.4
    skipped: list[str]                        # missing-capability skips (Req 5.7)

class BenchmarkFramework:
    def replay(self, cfg: RunConfig) -> RunReport: ...      # FAST25 replay (Req 5.1, 5.2)
    @staticmethod
    def percentiles(samples: list[float]) -> Percentiles: ...   # pure metric fn (Req 5.3)
    @staticmethod
    def rates(access_log: AccessLog) -> tuple[float, float]: ...# hit/reuse (Req 5.4)
```

The framework runs both the Project and the Baseline on the same workload/hardware (Req 5.1),
replays FAST25 trace records (timestamp, input_length, output_length, hash_ids), writes provenance
(config, software versions, pinned commit) into every report (Req 5.5), and when a required hardware
capability is absent it records the missing capability and skips the affected benchmark (Req 5.7).
Reproducibility (Req 5.6) is supported by fixed seeds, pinned versions, and a documented tolerance
band asserted across repeated runs.

## Data Models

### KVCache_Block

```cpp
// Fixed-granularity unit of key/value cache identified by a content hash / block key.
struct BlockKey {
  uint64_t hash_id;        // content hash (maps to FAST25 trace hash_ids)
  uint32_t layer;          // transformer layer index
  uint16_t version = 1;    // serialization/schema version
  bool operator==(const BlockKey&) const = default;
};

struct KVCacheBlock {
  BlockKey            key;
  uint32_t            num_tokens;     // tokens covered by this block
  uint16_t            num_heads;
  uint16_t            head_dim;
  DType               dtype;          // fp16/bf16/fp8/...
  std::vector<std::byte> k_payload;   // contiguous K tensor bytes
  std::vector<std::byte> v_payload;   // contiguous V tensor bytes
  bool operator==(const KVCacheBlock&) const = default;   // structural equality for round-trip
};

// Wire/storage form produced by Data_Path::Serialize.
struct SerializedBlock {
  std::vector<std::byte> bytes;       // [header | shape/dtype | K | V], length-prefixed, versioned
};
```

### Tier Metadata

```cpp
struct AccessStats {
  uint64_t access_count   = 0;     // frequency
  uint64_t last_access_ts = 0;     // recency (logical clock)
};

struct TierEntry {
  BlockKey   key;
  Tier       tier;
  size_t     size_bytes;
  AccessStats stats;               // updated on read, drives promotion/eviction (Req 2.3,2.4,2.6)
};

struct TierOccupancy {
  size_t used_bytes = 0;
  size_t capacity_bytes = 0;       // high_water = capacity_bytes * high_water_ratio
};
```

### Cross-Node Index

```cpp
struct IndexEntry {
  BlockKey            key;
  std::set<NodeId>    holders;     // nodes currently holding the block (Req 4.2)
  uint64_t            updated_ts;
};

struct NodeState {
  NodeId   id;
  bool     reachable = true;       // failover input (Req 4.5)
  double   occupancy_ratio = 0.0;  // high-water input (Req 4.4)
  TenantId owner_hint;             // for fairness accounting (Req 4.3)
};
```

## Error Handling

| Condition | Component | Behavior | Requirement |
|---|---|---|---|
| Block absent from all tiers | Tiered_Storage_Manager | Return miss (`LookupResult.hit=false`) to caller | 2.5 |
| Tier at/above threshold on write | Eviction_Policy | Evict victims (demote/remove) then place; never exceed capacity | 2.4 |
| Transfer fails on selected path | Data_Path | Return `TransferError{failing_key, path, reason}` | 3.6 |
| RDMA unavailable | Data_Path | Select TCP fallback path | 3.7 |
| Holder node unreachable | Scheduler | Route to alternative reachable holder, else signal recompute | 4.5 |
| Required HW capability missing | Benchmark_Framework | Record missing capability, skip affected benchmark | 5.7 |
| Pinned Mooncake version unavailable | Build/Adapter | Fail build with message naming required version | 6.5 |
| Concurrent shared-state access | Data_Path/Scheduler | Serialize via locks/lock-free structures; consistent transitions | 7.2 |
| Buffer/allocation acquired | Data_Path | RAII guarantees deregistration/free on all paths | 7.3 |

All C++ APIs return `Result<T>`/`Status` (no exceptions across the adapter boundary); Python wraps
these into typed exceptions for orchestration code.

## Concurrency and Memory-Safety Design

- **Shared-state serialization (Req 7.2).** Tier metadata maps and the cross-node index are guarded
  by fine-grained locks (per-shard mutexes) or lock-free structures with documented invariants. Each
  operation transitions shared state atomically so observers never see a torn or invalid state. A
  serial reference model is maintained for property testing.
- **RDMA hot path (Req 7.3).** Registrations and allocations use RAII (`ScopedRegistration`,
  owned buffers). A tracking allocator counts acquisitions vs. releases; the invariant is
  `acquired == released` at the end of every transfer sequence, including error/exception paths.
- **Sanitizer/static-analysis gate (Req 7.4).** CI runs ThreadSanitizer and AddressSanitizer plus
  static analysis (clang-tidy with concurrency checks) over the core transfer and concurrency paths.

## Testing Strategy

A dual approach is used: **property-based tests** for universal behaviors over generated inputs, and
**example/integration/smoke tests** for specific scenarios, external services, and configuration.

- **Property tests** (C++ via RapidCheck, Python via Hypothesis), minimum 100 iterations each, each
  tagged `Feature: mooncake-kvcache-optimization, Property {n}: {text}`. These cover serialization
  round-trip, tier placement/eviction invariants, cross-node index round-trip, routing, fairness,
  load balancing, failover, metric computation, report provenance, concurrency consistency, and
  resource release.
- **Example/edge tests** cover async-handle semantics (3.4), cache miss (2.5), NVMe-disabled
  operation (2.7), TCP fallback selection (3.7), prefix-reuse-disabled routing (4.6),
  missing-capability skip (5.7), and the build-failure message (6.5).
- **Integration tests** (1–3 representative examples) cover RDMA/GPUDirect/zero-copy paths (3.1–3.3),
  the vLLM KV connector wiring (6.1), end-to-end Project-vs-Baseline runs (5.1), and reproducibility
  tolerance across repeats (5.6).
- **Memory/thread-safety analysis** on the RDMA and concurrency hot paths via ASan/TSan + static
  analysis in CI (7.4), plus a randomized-schedule concurrency property and a tracking-allocator
  leak property.
- **Smoke/compliance checks** cover the Requirement-1 analysis artifact, pinned-commit recording,
  submodule integrity, license/attribution, and deliverable presence.

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a
system — essentially, a formal statement about what the system should do. Properties serve as the
bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: Single-tier placement invariant

For any sequence of write, read, prefetch, and eviction operations, every KVCache_Block that is
present resolves to exactly one authoritative tier location, and that location agrees with the tier
metadata.

**Validates: Requirements 2.1**

### Property 2: Threshold-respecting placement

For any tier whose occupancy is below its configured capacity threshold, writing a KVCache_Block
targeted at that tier places the block in that tier.

**Validates: Requirements 2.2**

### Property 3: Read records access signals and enables promotion

For any KVCache_Block read from a slower tier, the block's access statistics (recency and frequency)
are updated to reflect the read, and any block subsequently flagged by access signals is promoted to
a faster tier with its content preserved.

**Validates: Requirements 2.3, 2.6**

### Property 4: Eviction preserves the capacity bound

For any sequence of writes that drives a tier to or above its capacity threshold, after eviction the
tier's occupancy does not exceed its configured capacity, and the evicted blocks are those selected
by the documented recency-and-frequency policy (demoted to the next slower tier or removed when none
exists).

**Validates: Requirements 2.4**

### Property 5: Cache miss for absent blocks

For any block key that is present in no tier, a read reports a cache miss to the caller; and for any
configuration without NVMe, all operations succeed across the HBM and DRAM tiers without error.

**Validates: Requirements 2.5, 2.7**

### Property 6: Serialization round-trip

For any valid KVCache_Block, deserializing its serialized representation reconstructs a KVCache_Block
equal to the original.

**Validates: Requirements 3.5**

### Property 7: Transfer error carries the failing block key

For any KVCache_Block transfer that fails on its selected path, the reported transfer error
references the exact failing block key.

**Validates: Requirements 3.6**

### Property 8: Path selection honors capability

For any transfer request, when RDMA-capable interfaces are unavailable the selected path is the TCP
fallback, and when they are available an RDMA/GPUDirect path is selected.

**Validates: Requirements 3.1, 3.7**

### Property 9: Prefix-aware routing reuses existing blocks

For any request whose token-prefix KVCache_Blocks already exist on some node, with prefix reuse
enabled, the routing decision selects a node that holds those prefix blocks; with prefix reuse
disabled, routing does not consult the prefix index.

**Validates: Requirements 4.1, 4.6**

### Property 10: Cross-node index round-trip

For any sequence of register and unregister operations, looking up a block key returns exactly the
set of nodes currently holding that block.

**Validates: Requirements 4.2**

### Property 11: Fair-share allocation

For any set of concurrent tenant demands, the allocation conforms to the documented fair-share
policy: no tenant receives more than its fair share while another tenant with unmet demand is
starved, and spare capacity is redistributed to tenants with residual demand.

**Validates: Requirements 4.3**

### Property 12: Load-balancing redirect

For any cluster state in which at least one node is below its high-water threshold, when some node is
at or above its high-water threshold the chosen placement node is one below its threshold.

**Validates: Requirements 4.4**

### Property 13: Failover resolution

For any cluster state where the preferred holder of a requested block is unreachable, the scheduler
resolves to a reachable alternative node that holds the block, or signals recomputation when no
reachable holder exists.

**Validates: Requirements 4.5**

### Property 14: Trace replay fidelity

For any FAST25_Trace input, parsing then producing the replay plan preserves each record's
timestamp, input length, output length, and hash ids, and preserves arrival ordering.

**Validates: Requirements 5.2**

### Property 15: Metric computation correctness and bounds

For any sample set of latencies and any access log, the reported latency percentiles satisfy
p50 ≤ p90 ≤ p99 and lie within the sample's min/max, and the reported Cache_Hit_Rate and Reuse_Rate
lie in [0, 1] and equal a reference computation over the same inputs.

**Validates: Requirements 5.3, 5.4**

### Property 16: Report provenance completeness

For any benchmark run configuration, the emitted run report contains the run configuration, the
software versions, and the pinned upstream Mooncake commit hash.

**Validates: Requirements 5.5**

### Property 17: Concurrency consistency of shared state

For any interleaving of concurrent operations over shared KVCache_Block state on the Data_Path or in
the Scheduler, the observed sequence of shared-state transitions is equivalent to some valid serial
execution (no torn or inconsistent states).

**Validates: Requirements 7.2**

### Property 18: Data-path resource conservation

For any sequence of buffer registrations, transfers, and releases on the Data_Path — including
sequences that encounter transfer errors — the number of RDMA registrations and memory allocations
acquired equals the number released when the sequence completes.

**Validates: Requirements 7.3**
