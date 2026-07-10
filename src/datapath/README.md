# src/datapath —— 数据路径（Data_Path）

KVCache_Block 的读写传输：零拷贝缓冲注册、经 Transfer Engine 适配层的 RDMA/GPUDirect、
异步 I/O 流水线、KV 序列化，以及 TCP 回退。所有资源遵循 RAII 释放纪律（需求 7.3）。

计划内容（见 design.md 与 tasks.md 任务 6）：
- `data_path.*`：`Serialize`/`Deserialize`（带长度前缀与版本号、可往返）、`TransferAsync`。
- 带背压的有界提交队列；失败时返回携带失败块键的 `TransferError`（需求 3.6）。
- `ScopedRegistration` 等 RAII 句柄，保证注册/分配在成功与错误路径上均被释放。

> 本任务（任务 1）仅创建目录与说明；具体实现由后续任务填充。
