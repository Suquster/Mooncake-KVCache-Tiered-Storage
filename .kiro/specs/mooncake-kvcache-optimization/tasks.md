# Implementation Plan: Mooncake KVCache Optimization

## Overview

This plan implements a standalone KVCache optimization Project layered on the upstream Mooncake
project (kvcache-ai/Mooncake) consumed as a pinned submodule. Work follows an analysis-first gate:
repository scaffolding and submodule pinning come first, then the documented upstream Mooncake
analysis (Requirement 1), which is the prerequisite input to the adapter layer and all component
designs. After the adapter boundary is in place, components are built in dependency order —
Tiered_Storage_Manager → Data_Path → Scheduler → vLLM_Adapter → Benchmark_Framework — followed by
the CI memory/thread-safety gates and the open-source compliance / contest deliverables.

Core storage and transfer paths are implemented in **C++** (property tests via RapidCheck); the
benchmark framework, orchestration, and vLLM integration are implemented in **Python** (property
tests via Hypothesis). Each property test runs a minimum of 100 iterations and is tagged
`Feature: mooncake-kvcache-optimization, Property {n}: {text}`.

## Tasks

- [x] 1. Repository scaffolding and Mooncake submodule pinning
  - Create the repo-root directory structure: `src/adapter`, `src/storage`, `src/datapath`,
    `src/scheduler`, `python/vllm_adapter`, `python/bench`, `docs/analysis`, `tests/`
  - Add the upstream Mooncake project as a Git submodule at `third_party/mooncake/`
  - Pin the submodule to a recorded commit and write `mooncake.lock` with the commit hash + version
  - Add the C++ build (CMake) and Python project (pyproject) skeletons, pybind11 binding stub, and
    the RapidCheck + Hypothesis test harness wiring
  - Add a build-time check that fails with a message naming the required Mooncake version when the
    pinned version is unavailable
  - _Requirements: 1.3, 6.2, 6.3, 6.5_

- [x] 1.1 Write smoke test for submodule integrity and build-failure message
  - Assert `mooncake.lock` records a commit hash + version and that the build fails with the
    required-version message when the pinned version is missing
  - _Requirements: 6.3, 6.5_

- [x] 2. Upstream Mooncake architecture analysis (analysis-first gate — must precede component design lock)
  - [x] 2.1 Document the Transfer_Engine surface and data-movement flow
    - In `docs/analysis/upstream-mooncake-analysis.md`, describe Transfer_Engine public interfaces,
      supported transport protocols (TCP/RDMA/GPUDirect/NVMe-oF/NVLink), and the
      register→submit→complete data-movement flow against the pinned commit's real headers
    - Record the pinned upstream commit hash and version used as the analysis baseline
    - _Requirements: 1.1, 1.3_

  - [x] 2.2 Document the Store_Layer surface and enumerate extension points
    - Describe Store_Layer public interfaces, the key/value object model, and multi-tier cache
      behavior; map KVCache_Block keys onto Store keys
    - Identify the Transfer_Engine and Store_Layer extension points the Tiered_Storage_Manager,
      Data_Path, and Scheduler will build upon, plus the Mooncake KV connector surface
    - Reconcile the provisional component interface signatures against this analysis and sign off
      the gate before downstream designs are locked
    - _Requirements: 1.2, 1.4, 1.5_

- [ ] 3. Adapter layer over upstream Mooncake (Project↔Mooncake boundary)
  - [-] 3.1 Define Project-owned core data models and adapter interface headers
    - Implement `BlockKey`, `KVCacheBlock`, `SerializedBlock` data models with structural equality
    - Define `ITransferBackend` (`RegisterBuffer`/`DeregisterBuffer`/`SelectPath`/`SubmitAsync`,
      `BufferHandle`, `TransferRequest`, `TransportPath`) and `IObjectStore`
      (`Put`/`Get`/`Exists`/`Locate`) in `src/adapter/`
    - _Requirements: 1.4, 6.2_

  - [~] 3.2 Implement the Transfer_Engine adapter over upstream Mooncake
    - Implement `ITransferBackend` in `transfer_engine_adapter.*`: zero-copy buffer registration,
      capability-based path selection (RDMA/GPUDirect else TCP), and async batched submit with
      completion futures; confine all upstream header includes to `src/adapter/*`
    - _Requirements: 1.4, 3.1, 3.2, 3.7_

  - [~] 3.3 Implement the Store adapter bridging KVCache_Block keys onto Mooncake Store
    - Implement `IObjectStore` in `store_adapter.*` mapping `BlockKey` onto Store keys and exposing
      `Locate` to back the cross-node index
    - _Requirements: 1.4, 6.2_

  - [~] 3.4 Write unit tests for adapter wiring with a fake backend
    - Exercise register/deregister and put/get/exists/locate against an in-memory fake to keep the
      boundary testable without hardware
    - _Requirements: 6.2_

- [ ] 4. Tiered_Storage_Manager: placement, eviction, and prefetch (C++)
  - [~] 4.1 Implement tier configuration, metadata, and the single-tier placement model
    - Implement `Tier`, `TierConfig`, `TierEntry`, `TierOccupancy`, `AccessStats`, `LookupResult`
      and the `Locate` placement invariant (a present block resolves to exactly one tier)
    - Implement `Write` honoring per-tier capacity thresholds in `src/storage/tiered_storage_manager.*`
    - _Requirements: 2.1, 2.2_

  - [~] 4.2 Write property test for single-tier placement invariant
    - **Property 1: Single-tier placement invariant**
    - **Validates: Requirements 2.1**

  - [~] 4.3 Write property test for threshold-respecting placement
    - **Property 2: Threshold-respecting placement**
    - **Validates: Requirements 2.2**

  - [~] 4.4 Implement read path, access-signal recording, and cache-miss / NVMe-disabled behavior
    - Implement `Read` returning hit/miss and updating recency+frequency on slower-tier hits
    - Ensure all operations succeed across HBM and DRAM when NVMe is not configured
    - _Requirements: 2.3, 2.5, 2.7_

  - [~] 4.5 Write property test for read access signals and promotion
    - **Property 3: Read records access signals and enables promotion**
    - **Validates: Requirements 2.3, 2.6**

  - [~] 4.6 Write property test for cache miss and NVMe-disabled operation
    - **Property 5: Cache miss for absent blocks**
    - **Validates: Requirements 2.5, 2.7**

  - [~] 4.7 Implement Eviction_Policy and capacity enforcement
    - Implement segmented-LRU-with-frequency `EnforceCapacity`: demote victims to the next slower
      tier, or remove when no slower tier exists, guaranteeing post-eviction occupancy <= capacity
    - _Requirements: 2.4_

  - [~] 4.8 Write property test for eviction capacity bound
    - **Property 4: Eviction preserves the capacity bound**
    - **Validates: Requirements 2.4**

  - [~] 4.9 Implement Prefetch_Engine promotions ahead of predicted reuse
    - Implement `Prefetch` consuming access signals (recency, frequency, prefix-reuse hints) to
      promote blocks to a faster tier with content preserved, reusing the Data_Path where possible
    - _Requirements: 2.6_

- [~] 5. Checkpoint - tiered storage
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 6. Data_Path: serialization, zero-copy, RDMA/GPUDirect, async pipeline, TCP fallback (C++)
  - [~] 6.1 Implement KV serialization and deserialization
    - Implement `Serialize`/`Deserialize` in `src/datapath/data_path.*` producing a length-prefixed,
      versioned `[header | shape/dtype | K | V]` form that round-trips to an equal KVCacheBlock
    - _Requirements: 3.5_

  - [~] 6.2 Write property test for serialization round-trip
    - **Property 6: Serialization round-trip**
    - **Validates: Requirements 3.5**

  - [~] 6.3 Implement zero-copy transfer with capability-based path selection and async interface
    - Implement `TransferAsync` over the Transfer_Engine adapter: zero-copy registered buffers,
      RDMA/GPUDirect when available else TCP fallback, returning a pending future before completion
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.7_

  - [~] 6.4 Write property test for path selection capability
    - **Property 8: Path selection honors capability**
    - **Validates: Requirements 3.1, 3.7**

  - [~] 6.5 Write example test for async-handle semantics and zero-copy/GPUDirect paths
    - Assert `TransferAsync` returns before completion (3.4) and that registered buffers are not
      copied through intermediate buffers (3.3); cover RDMA/GPUDirect path selection (3.1, 3.2)
    - _Requirements: 3.1, 3.2, 3.3, 3.4_

  - [~] 6.6 Implement async I/O pipeline with backpressure and failing-key error reporting
    - Implement the bounded submission queue feeding `SubmitAsync` with completion delivery and
      backpressure; on failure return `TransferError{failing_key, path, reason}`
    - _Requirements: 3.4, 3.6_

  - [~] 6.7 Write property test for transfer error carrying the failing key
    - **Property 7: Transfer error carries the failing block key**
    - **Validates: Requirements 3.6**

  - [~] 6.8 Implement RAII resource discipline on the data path
    - Implement `ScopedRegistration` and RAII-owned buffers so every `RegisterBuffer` is paired with
      `DeregisterBuffer` and all allocations are released on success and error paths
    - _Requirements: 7.3_

  - [~] 6.9 Write property test for data-path resource conservation
    - **Property 18: Data-path resource conservation**
    - **Validates: Requirements 7.3**

- [~] 7. Checkpoint - data path
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 8. Scheduler: cross-node index, routing, fairness, load balancing, failover (C++)
  - [~] 8.1 Implement the cross-node index
    - Implement `CrossNodeIndex` (`Register`/`Unregister`/`Lookup`) mapping block keys to current
      holder sets, plus `IndexEntry`/`NodeState` in `src/scheduler/scheduler.*`
    - _Requirements: 4.2_

  - [~] 8.2 Write property test for cross-node index round-trip
    - **Property 10: Cross-node index round-trip**
    - **Validates: Requirements 4.2**

  - [~] 8.3 Implement prefix-aware routing with config toggle
    - Implement `Route` selecting a node holding existing prefix blocks when reuse is enabled, and
      not consulting the prefix index when reuse is disabled by configuration
    - _Requirements: 4.1, 4.6_

  - [~] 8.4 Write property test for prefix-aware routing
    - **Property 9: Prefix-aware routing reuses existing blocks**
    - **Validates: Requirements 4.1, 4.6**

  - [~] 8.5 Implement fair-share allocation and load-balancing placement
    - Implement `AllocateFairShare` (weighted max-min fairness) and `ChoosePlacementNode` steering
      placement to below-high-water nodes when any node is at/above high-water
    - _Requirements: 4.3, 4.4_

  - [~] 8.6 Write property test for fair-share allocation
    - **Property 11: Fair-share allocation**
    - **Validates: Requirements 4.3**

  - [~] 8.7 Write property test for load-balancing redirect
    - **Property 12: Load-balancing redirect**
    - **Validates: Requirements 4.4**

  - [~] 8.8 Implement failover resolution
    - Implement `ResolveOnFailure` selecting a reachable alternative holder from the index, or
      signaling recomputation when no reachable holder exists
    - _Requirements: 4.5_

  - [~] 8.9 Write property test for failover resolution
    - **Property 13: Failover resolution**
    - **Validates: Requirements 4.5**

- [ ] 9. Concurrency consistency for shared scheduler/data-path state
  - [~] 9.1 Implement shared-state serialization for tier metadata and the cross-node index
    - Guard shared maps with per-shard mutexes or documented lock-free structures so each operation
      transitions state atomically; maintain a serial reference model for testing
    - _Requirements: 7.2_

  - [~] 9.2 Write property test for concurrency consistency of shared state
    - **Property 17: Concurrency consistency of shared state**
    - **Validates: Requirements 7.2**

- [~] 10. Checkpoint - scheduler and concurrency
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 11. vLLM_Adapter via the Mooncake KV connector (Python)
  - [~] 11.1 Implement the Mooncake KV connector integration
    - Implement `ProjectKVConnector` (`store_kv`/`load_kv`/`supports`) translating vLLM KV-connector
      callbacks into Scheduler routing + Data_Path transfers over the pybind11 binding, exposing the
      tiered store as the connector's backing pool with no upstream source modification
    - _Requirements: 6.1, 6.2, 6.3_

  - [~] 11.2 Write integration test for the KV connector wiring
    - Exercise store_kv/load_kv through the connector against the C++ core via a fake/in-memory
      backend to verify the connector contract
    - _Requirements: 6.1_

- [ ] 12. Benchmark_Framework: FAST25 replay, baseline comparison, metrics (Python)
  - [~] 12.1 Implement FAST25 trace parsing and replay-plan construction
    - Implement trace parsing/replay-plan construction in `python/bench/framework.py` preserving each
      record's timestamp, input_length, output_length, hash_ids, and arrival ordering
    - _Requirements: 5.2_

  - [~] 12.2 Write property test for trace replay fidelity
    - **Property 14: Trace replay fidelity**
    - **Validates: Requirements 5.2**

  - [~] 12.3 Implement metric computation functions
    - Implement `percentiles` (p50/p90/p99) and `rates` (Cache_Hit_Rate, Reuse_Rate) as pure
      functions over latency samples and the access log
    - _Requirements: 5.3, 5.4_

  - [~] 12.4 Write property test for metric computation correctness and bounds
    - **Property 15: Metric computation correctness and bounds**
    - **Validates: Requirements 5.3, 5.4**

  - [~] 12.5 Implement end-to-end run orchestration, report provenance, and capability skips
    - Implement `replay` running Project and Baseline on the same workload/hardware, writing
      `RunReport` with config, software versions, and pinned Mooncake commit; record and skip
      benchmarks when a required hardware capability is absent; support reproducibility via fixed
      seeds, pinned versions, and a documented tolerance band
    - _Requirements: 5.1, 5.5, 5.6, 5.7_

  - [~] 12.6 Write property test for report provenance completeness
    - **Property 16: Report provenance completeness**
    - **Validates: Requirements 5.5**

  - [~] 12.7 Write example/integration tests for E2E run, capability skip, and reproducibility
    - Cover a Project-vs-Baseline run on the same workload (5.1), missing-capability skip (5.7), and
      metric values within the tolerance band across repeated runs (5.6)
    - _Requirements: 5.1, 5.6, 5.7_

- [~] 13. Checkpoint - integration and benchmarking
  - Ensure all tests pass, ask the user if questions arise.

- [ ] 14. CI memory-safety and thread-safety gates
  - [~] 14.1 Configure ASan/TSan and static-analysis CI jobs on core paths
    - Add CI jobs running AddressSanitizer, ThreadSanitizer, and clang-tidy concurrency checks over
      the transfer and concurrency hot paths, applying Mooncake C++/Python coding conventions
    - _Requirements: 7.1, 7.4_

  - [~] 14.2 Add a randomized-schedule concurrency job and tracking-allocator leak job
    - Wire the concurrency property and the data-path tracking-allocator leak property into CI so
      sanitizer findings fail the build
    - _Requirements: 7.4_

- [ ] 15. Open-source compliance and contest deliverables
  - [~] 15.1 Add license, third-party declarations, and attribution documentation
    - Add an open-source license; declare source/license/dependency relationship for each
      third-party component including upstream Mooncake; satisfy attribution/redistribution
      obligations in repository docs
    - _Requirements: 8.1, 8.2, 8.5_

  - [~] 15.2 Add setup/reproduction instructions and assemble submission deliverables
    - Write build + benchmark + metric-reproduction instructions for the target hardware, and
      reference the source code, design document, and (placeholder for) the ≤5-minute demo video in
      the submission deliverables
    - _Requirements: 8.3, 8.4_

  - [~] 15.3 Write compliance smoke checks
    - Assert presence of the Requirement-1 analysis artifact, pinned-commit recording, license,
      third-party declarations, and required deliverables
    - _Requirements: 8.1, 8.2, 8.3, 8.4, 8.5_

- [~] 16. Final checkpoint - ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional test sub-tasks and can be skipped for a faster MVP.
- Task 2 (upstream Mooncake analysis) is the analysis-first gate: it must be completed before the
  adapter layer and component designs are locked (Requirement 1.5).
- All 18 correctness properties from the design are covered by dedicated property-test sub-tasks
  (Properties 1–18), each annotated with its property number and the requirement clause it checks.
- Property tests use RapidCheck (C++) and Hypothesis (Python), minimum 100 iterations, tagged
  `Feature: mooncake-kvcache-optimization, Property {n}: {text}`.
- Checkpoints ensure incremental validation at each major component boundary.

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["2.1"] },
    { "id": 2, "tasks": ["2.2"] },
    { "id": 3, "tasks": ["3.1"] },
    { "id": 4, "tasks": ["3.2", "3.3"] },
    { "id": 5, "tasks": ["3.4", "4.1", "6.1", "8.1", "12.1", "12.3"] },
    { "id": 6, "tasks": ["4.2", "4.3", "4.4", "6.2", "6.3", "8.2", "8.3", "12.2", "12.4"] },
    { "id": 7, "tasks": ["4.5", "4.6", "4.7", "6.4", "6.5", "6.6", "8.4", "8.5", "12.5"] },
    { "id": 8, "tasks": ["4.8", "4.9", "6.7", "6.8", "8.6", "8.7", "8.8", "12.6", "12.7"] },
    { "id": 9, "tasks": ["6.9", "8.9", "9.1", "11.1"] },
    { "id": 10, "tasks": ["9.2", "11.2", "14.1", "15.1"] },
    { "id": 11, "tasks": ["14.2", "15.2"] },
    { "id": 12, "tasks": ["15.3"] }
  ]
}
```
