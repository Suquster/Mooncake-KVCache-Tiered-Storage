# docs/analysis —— 上游 Mooncake 架构分析（需求 1，分析先行门禁）

本目录承载「分析先行」工件：在锁定 Tiered_Storage_Manager、Data_Path、Scheduler 的设计之前，
必须先完成对上游 Mooncake Transfer Engine 与 Mooncake Store 的接口、传输协议、数据流动、
键值对象模型、多层缓存行为与扩展点的成文分析，并记录锁定的上游 commit 哈希（需求 1.5）。

工件（见 tasks.md 任务 2）：
- `upstream-mooncake-analysis.md`：Transfer_Engine / Store_Layer 接口与扩展点分析，
  对应锁定基线 commit `356d99fb28746d274241b6792c2f7c2fe17e3b29`（v0.3.6.post1）。

进度：
- ✅ 任务 2.1：已完成 **Transfer_Engine 面**分析——公开接口、支持的传输协议
  （TCP / RDMA / GPUDirect / NVMe-oF / NVLink 等）、`register→submit→complete` 数据流，
  并记录锁定基线 commit/version（需求 1.1、1.3）。
- ⏳ 任务 2.2：待补充 **Store_Layer 面**分析、键值对象模型、多层缓存行为、扩展点完整枚举与门禁
  签收（需求 1.2、1.4、1.5）。
