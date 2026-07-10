// =============================================================================
// src/adapter/store_adapter.h —— Project↔Mooncake 对象存储适配接口
// =============================================================================
// 设计依据：design.md「Project↔Mooncake Adapter Interfaces」，并已与
// docs/analysis/upstream-mooncake-analysis.md §11.2 对账锁定（GATE: PASS）。
//
// 本头文件**仅定义项目自有抽象**（IObjectStore），不包含上游 Mooncake 头文件；
// 把 KVCache_Block 键（结构化 BlockKey）桥接到上游 Store 的不透明 ObjectKey
// （单射可逆编码见分析文档 §9.3），并隔离上游 API 演进（需求 6.2）。
//
// 上游支撑（分析文档 §11.2 / §10.2）：
//   Put    ← Client::Put(ObjectKey, vector<Slice>&, ReplicateConfig)
//   Get    ← Client::Get(ObjectKey, vector<Slice>&)（+ 先 Query）
//   Exists ← Client::IsExist(ObjectKey)
//   Locate ← Client::Query → Replica::Descriptor → segment_name（=节点 ip:port）
// =============================================================================
#ifndef PROJECT_ADAPTER_STORE_ADAPTER_H_
#define PROJECT_ADAPTER_STORE_ADAPTER_H_

#include <vector>

#include "project/status.h"
#include "project/types.h"

namespace project::adapter {

// 对象存储抽象接口：在上游 Mooncake Store_Layer 之上实现（任务 3.3）。
// 所有方法以 Result<T>/Status 报告成败，**不跨边界抛异常**（design.md「Error Handling」）。
// 键映射：BlockKey ⇄ ObjectKey 经确定性、单射可逆编码桥接（分析文档 §9.3）。
class IObjectStore {
 public:
  virtual ~IObjectStore() = default;

  // 写入：把序列化块以一个/多个 Slice 形式写入 Store（落数到内存/磁盘副本）。
  // 容量不足时上游返回 NO_AVAILABLE_HANDLE，归一化为 kOutOfCapacity（需求 2.4）。
  virtual Status Put(const BlockKey& key, const SerializedBlock& block) = 0;

  // 读取：返回序列化块；对象不存在时返回 kNotFound（缓存未命中，需求 2.5）。
  virtual Result<SerializedBlock> Get(const BlockKey& key) = 0;

  // 存在性探测（不搬数据）。对应上游 IsExist（OBJECT_NOT_FOUND → false）。
  virtual bool Exists(const BlockKey& key) const = 0;

  // 定位：返回当前持有该块的节点集合，回填 Scheduler 的 CrossNodeIndex（需求 4.2）。
  // 由上游 Query 返回的副本描述符 segment_name（= 节点 ip:port）解析得到。
  virtual std::vector<NodeId> Locate(const BlockKey& key) const = 0;
};

}  // namespace project::adapter

#endif  // PROJECT_ADAPTER_STORE_ADAPTER_H_
