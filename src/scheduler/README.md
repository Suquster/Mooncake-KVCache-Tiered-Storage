# src/scheduler —— 跨节点调度（Scheduler，控制平面）

跨节点前缀感知复用、多租户公平分配、负载均衡与故障转移。作为控制平面，调度器只做
路由/索引/公平性决策，绝不拷贝块负载（块负载移动一律走 Data_Path）。

计划内容（见 design.md 与 tasks.md 任务 8）：
- `CrossNodeIndex`：`Register`/`Unregister`/`Lookup`，块键→当前持有节点集合（需求 4.2）。
- `Route`：前缀感知路由，可由配置关闭（需求 4.1 / 4.6）。
- `AllocateFairShare`：加权 max-min 公平（需求 4.3）；`ChoosePlacementNode`：高水位负载均衡（需求 4.4）。
- `ResolveOnFailure`：不可达持有者→可达备选或触发重算（需求 4.5）。

> 本任务（任务 1）仅创建目录与说明；具体实现由后续任务填充。
