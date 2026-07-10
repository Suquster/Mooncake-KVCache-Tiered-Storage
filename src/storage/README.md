# src/storage —— 分层存储管理（Tiered_Storage_Manager）

跨 HBM→DRAM→NVMe 的 KVCache_Block 放置与迁移，驱动预取（Prefetch_Engine）与
淘汰（Eviction_Policy）。在未配置 NVMe 时仍能在 HBM/DRAM 两层正确工作（需求 2.7）。

计划内容（见 design.md 与 tasks.md 任务 4）：
- `tiered_storage_manager.*`：`Tier`、`TierConfig`、`TierEntry`、单层放置不变量、
  `Write`/`Read`/`Locate`/`EnforceCapacity`/`Prefetch`。
- 淘汰策略：带频率加权的分段 LRU，保证淘汰后占用 ≤ 配置容量。

> 本任务（任务 1）仅创建目录与说明；具体实现由后续任务填充。
