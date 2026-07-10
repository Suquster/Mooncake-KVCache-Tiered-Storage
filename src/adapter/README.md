# src/adapter —— Project↔Mooncake 适配层

**唯一允许包含上游 Mooncake 头文件 / 链接其库的目录。** 其余所有模块只依赖本目录暴露的
Project 自有抽象（`ITransferBackend`、`IObjectStore`），从而把上游耦合面收敛到最小、便于审计，
并在锁定版本不可用时让构建以明确信息失败（需求 6.5）。

计划内容（见 design.md 与 tasks.md 任务 3）：
- `transfer_engine_adapter.*`：基于 Mooncake Transfer Engine 实现零拷贝缓冲注册、
  基于能力的路径选择（RDMA/GPUDirect，否则 TCP 回退）、异步批量提交。
- `store_adapter.*`：将 `BlockKey` 映射到 Mooncake Store 键，暴露 `Locate` 支撑跨节点索引。

> 本任务（任务 1）仅创建目录与说明；具体实现由后续任务填充。
