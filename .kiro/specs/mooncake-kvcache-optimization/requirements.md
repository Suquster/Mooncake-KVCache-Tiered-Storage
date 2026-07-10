# Requirements Document

## Introduction

This feature defines a KVCache optimization project built on top of the upstream Mooncake project (kvcache-ai/Mooncake) for the 2026 CCF open-source contest (Track 2). The work is delivered as a standalone Git repository that depends on Mooncake as a library/submodule with adapter layers, packaged for "file + link" submission (not an in-tree upstream pull request).

The project spans four integrated directions: (1) a tiered KVCache storage architecture (HBM→DRAM→NVMe) with intelligent prefetch and eviction; (2) data-path performance optimization (zero-copy transfer, RDMA/GPUDirect, asynchronous I/O pipeline, optimized KV serialization); (3) cross-node scheduling and load balancing with prefix-aware reuse and multi-tenant fair scheduling; and (4) a reproducible benchmark framework that evaluates the system against vLLM native PagedAttention/prefix-cache baselines. The core storage and transfer paths are implemented in C++; benchmarking, orchestration, and vLLM integration are implemented in Python.

Work begins with an analysis of the actual upstream Mooncake Transfer Engine and Mooncake Store architecture and code structure, which informs all subsequent design. The implementation must satisfy contest judging priorities (innovation, technical completeness, scenario fit, open-source compliance) and treat reproducibility and open-source compliance as first-class requirements.

## Glossary

- **Project**: The KVCache optimization system delivered by this work, comprising the tiered storage manager, data-path optimizations, scheduler, adapters, and benchmark framework.
- **Mooncake**: The upstream open-source project (kvcache-ai/Mooncake) consumed as a library/submodule dependency.
- **Transfer_Engine**: The Mooncake Transfer Engine component responsible for batched data movement across storage, network, and accelerator backends.
- **Store_Layer**: The Mooncake Store component providing distributed key/value KVCache storage on top of the Transfer Engine.
- **Tiered_Storage_Manager**: The Project component that manages KVCache placement and movement across the HBM, DRAM, and NVMe tiers.
- **Prefetch_Engine**: The Project component that proactively loads KVCache blocks into a faster tier before they are requested.
- **Eviction_Policy**: The Project component that selects KVCache blocks to demote or remove when a tier reaches a capacity threshold.
- **Data_Path**: The Project components implementing KVCache read/write transfer, including zero-copy, RDMA/GPUDirect, asynchronous I/O, and serialization.
- **Scheduler**: The Project component that performs cross-node KVCache scheduling, prefix-aware reuse, and multi-tenant fair allocation.
- **vLLM_Adapter**: The Project adapter that integrates the system with vLLM through the Mooncake KV connector interface.
- **Benchmark_Framework**: The Project component that runs reproducible end-to-end benchmarks and produces metrics reports.
- **Baseline**: vLLM native PagedAttention with its built-in prefix cache, used as the comparison reference.
- **HBM**: GPU high-bandwidth memory tier.
- **DRAM**: Host main-memory tier.
- **NVMe**: NVMe SSD storage tier.
- **KVCache_Block**: A fixed-granularity unit of key/value cache identified by a content hash or block key.
- **TTFT**: Time-to-first-token latency.
- **Cache_Hit_Rate**: The fraction of requested KVCache blocks served from any tier of the Project without recomputation.
- **Reuse_Rate**: The fraction of input tokens whose KVCache is reused across requests rather than recomputed.
- **FAST25_Trace**: The official Mooncake FAST'25 workload trace file (mooncake_trace.jsonl) used for workload replay.
- **Maintainer**: A developer building, configuring, or extending the Project.
- **Operator**: A person deploying and running the Project on the target cluster.

## Requirements

### Requirement 1: Upstream Mooncake architecture analysis

**User Story:** As a Maintainer, I want a documented analysis of the upstream Mooncake Transfer Engine and Store architecture and code structure, so that all subsequent design decisions are grounded in the actual upstream implementation.

#### Acceptance Criteria

1. THE Project SHALL produce an architecture analysis document that describes the Transfer_Engine public interfaces, supported transport protocols, and data-movement flow.
2. THE Project SHALL produce an architecture analysis document that describes the Store_Layer public interfaces, key/value object model, and multi-tier cache behavior.
3. THE Project SHALL record the pinned upstream Mooncake commit hash and version used as the dependency baseline for the analysis.
4. THE Project SHALL identify the Transfer_Engine and Store_Layer extension points that the Tiered_Storage_Manager, Data_Path, and Scheduler will build upon.
5. THE architecture analysis document SHALL be completed before the design of the Tiered_Storage_Manager, Data_Path, and Scheduler is finalized.

### Requirement 2: Tiered KVCache storage architecture

**User Story:** As an Operator, I want KVCache stored across HBM, DRAM, and NVMe tiers with automatic placement and movement, so that cache capacity is larger than HBM alone while keeping hot data fast to access.

#### Acceptance Criteria

1. THE Tiered_Storage_Manager SHALL maintain KVCache_Block placement across the HBM, DRAM, and NVMe tiers.
2. WHEN a KVCache_Block is written and its target tier is below its configured capacity threshold, THE Tiered_Storage_Manager SHALL place the KVCache_Block in that tier.
3. WHEN a KVCache_Block is read from a slower tier, THE Tiered_Storage_Manager SHALL record the access so that promotion decisions can use access recency and frequency.
4. WHILE a tier occupancy is at or above its configured capacity threshold, THE Eviction_Policy SHALL select KVCache_Blocks for demotion to the next slower tier or removal according to a documented recency-and-frequency policy.
5. IF a requested KVCache_Block is not present in any tier, THEN THE Tiered_Storage_Manager SHALL report a cache miss to the caller.
6. THE Prefetch_Engine SHALL promote KVCache_Blocks from a slower tier to a faster tier ahead of predicted reuse based on documented access signals.
7. WHERE the NVMe tier is not configured, THE Tiered_Storage_Manager SHALL operate across the HBM and DRAM tiers without error.

### Requirement 3: Data-path performance optimization

**User Story:** As an Operator, I want KVCache transfers to use zero-copy, RDMA/GPUDirect, and asynchronous I/O, so that cross-node memory and network I/O is fast and CPU overhead is low.

#### Acceptance Criteria

1. WHERE RDMA-capable network interfaces are available, THE Data_Path SHALL transfer KVCache_Blocks over RDMA through the Transfer_Engine.
2. WHERE GPUDirect is available on the target GPUs, THE Data_Path SHALL transfer KVCache_Blocks directly between GPU memory and the network or storage backend without an intermediate host-memory copy.
3. THE Data_Path SHALL perform KVCache_Block transfers using a zero-copy buffer-registration path that avoids copying block payloads through additional intermediate buffers.
4. THE Data_Path SHALL expose an asynchronous I/O interface that allows a caller to submit a KVCache_Block transfer and continue execution before the transfer completes.
5. WHEN a KVCache_Block is serialized for storage or transfer, THE Data_Path SHALL produce a representation that the corresponding deserialization step reconstructs to an equal KVCache_Block.
6. IF a KVCache_Block transfer fails on its selected path, THEN THE Data_Path SHALL report a transfer error to the caller with the failing block key.
7. WHERE RDMA-capable interfaces are unavailable, THE Data_Path SHALL transfer KVCache_Blocks over a TCP fallback path.

### Requirement 4: Cross-node scheduling and load balancing

**User Story:** As an Operator, I want cross-node KVCache scheduling with prefix-aware reuse and fair multi-tenant allocation, so that cache is reused across nodes and tenants share capacity fairly.

#### Acceptance Criteria

1. WHEN a request arrives with a token prefix whose KVCache_Blocks already exist on any node, THE Scheduler SHALL route the request to reuse the existing KVCache_Blocks.
2. THE Scheduler SHALL maintain a cross-node index that maps KVCache_Block keys to the nodes holding them.
3. WHILE multiple tenants submit requests concurrently, THE Scheduler SHALL allocate cache capacity among tenants according to a documented fair-share policy.
4. WHEN a node's cache occupancy exceeds its configured high-water threshold, THE Scheduler SHALL direct new KVCache_Block placement toward nodes below their high-water threshold.
5. IF a node holding requested KVCache_Blocks is unreachable, THEN THE Scheduler SHALL fall back to recomputation or an alternative node holding the same KVCache_Blocks.
6. WHERE prefix-aware reuse is disabled by configuration, THE Scheduler SHALL route requests without consulting the cross-node prefix index.

### Requirement 5: Reproducible benchmarking and evaluation

**User Story:** As a Maintainer, I want a reproducible benchmark framework that compares the Project against the vLLM PagedAttention baseline, so that performance claims are verifiable and repeatable.

#### Acceptance Criteria

1. THE Benchmark_Framework SHALL run end-to-end benchmarks of the Project and of the Baseline using the same workload and hardware configuration.
2. THE Benchmark_Framework SHALL replay workloads from the FAST25_Trace file.
3. THE Benchmark_Framework SHALL report output token throughput, TTFT, and end-to-end latency percentiles including the 50th, 90th, and 99th percentiles.
4. THE Benchmark_Framework SHALL report Cache_Hit_Rate and Reuse_Rate for each Project benchmark run.
5. THE Benchmark_Framework SHALL write each benchmark run's configuration, software versions, and pinned upstream Mooncake commit hash into the run's output report.
6. WHEN the same benchmark configuration and the same FAST25_Trace input are executed on the same hardware, THE Benchmark_Framework SHALL produce reported metric values within a documented tolerance band across repeated runs.
7. IF a required hardware capability for a benchmark is unavailable, THEN THE Benchmark_Framework SHALL report the missing capability and skip the affected benchmark.

### Requirement 6: vLLM integration and Mooncake compatibility

**User Story:** As an Operator, I want the Project to integrate with vLLM through the Mooncake KV connector and depend on upstream Mooncake as a submodule, so that the system runs in a real vLLM inference deployment.

#### Acceptance Criteria

1. THE vLLM_Adapter SHALL connect the Project to vLLM through the Mooncake KV connector interface.
2. THE Project SHALL depend on Mooncake as a library/submodule and SHALL NOT modify the upstream Mooncake source tree in place.
3. WHEN the Project is built, THE Project SHALL link against the pinned upstream Mooncake version recorded in the dependency configuration.
4. THE Project SHALL run on a Linux operating system with NVIDIA GPUs, RDMA/RoCE network interfaces, and NVMe SSDs.
5. IF the configured upstream Mooncake version is unavailable at build time, THEN THE Project SHALL fail the build with a message that names the required Mooncake version.

### Requirement 7: Coding standards, memory safety, and thread safety

**User Story:** As a Maintainer, I want the core C++ transfer and concurrency paths to follow Mooncake coding conventions and strict memory and thread-safety standards, so that the RDMA and concurrency hot paths are correct and maintainable.

#### Acceptance Criteria

1. THE Project SHALL follow the Mooncake C++ and Python coding conventions for source files contributed by the Project.
2. WHILE multiple threads access shared KVCache_Block state on the Data_Path or in the Scheduler, THE Project SHALL serialize access so that shared state transitions remain consistent.
3. THE Project SHALL release every RDMA buffer registration and memory allocation that the Project acquires on the Data_Path.
4. THE Project SHALL pass a memory-safety and thread-safety analysis configured in the Project's continuous integration on the core transfer and concurrency paths.

### Requirement 8: Open-source compliance and contest deliverables

**User Story:** As a Maintainer, I want the Project to meet open-source compliance rules and produce the required contest deliverables, so that the submission is eligible for judging.

#### Acceptance Criteria

1. THE Project SHALL be hosted in a public GitHub repository under an open-source license.
2. THE Project SHALL declare the source, license, and dependency relationship for each third-party component, including upstream Mooncake, that the Project uses.
3. THE Project SHALL include source code, a design document or presentation, and a demonstration video of at most five minutes in the submission deliverables.
4. THE Project SHALL provide setup and reproduction instructions that allow an Operator to build the Project, run a benchmark, and reproduce reported metrics on the target hardware.
5. IF a third-party component's license imposes attribution or redistribution obligations, THEN THE Project SHALL satisfy those obligations in the repository documentation.
