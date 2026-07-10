// =============================================================================
// project/types.h —— 项目自有的核心数据模型
// =============================================================================
// 设计依据（design.md「Data Models」）：定义 KVCache 的固定粒度单元及其键、
// 序列化形态、节点标识与数据类型。这些类型为「项目自有」，与上游 Mooncake 解耦，
// 仅在 src/adapter/* 边界处与上游 ObjectKey / Slice 等互转
// （映射规范见 docs/analysis/upstream-mooncake-analysis.md §9.3）。
//
// 关键不变量：
//   * BlockKey / KVCacheBlock / SerializedBlock 提供「结构相等」——支撑序列化往返
//     属性（design.md Property 6，需求 3.5）。
//   * 所有结构化信息使用精确类型（枚举/定宽整数），禁止以字符串模拟结构化数据。
// =============================================================================
#ifndef PROJECT_TYPES_H_
#define PROJECT_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace project {

// ---------------------------------------------------------------------------
// 命名常量（禁止散落魔数）
// ---------------------------------------------------------------------------
// BlockKey 默认 schema 版本。序列化/键编码演进时递增以隔离键空间，避免脏读。
inline constexpr std::uint16_t kBlockSchemaVersion = 1;

// ---------------------------------------------------------------------------
// 节点标识 NodeId
// ---------------------------------------------------------------------------
// 上游 Mooncake 以 Segment.name（通常为 "ip:port"）定位副本所在节点
// （分析文档 §9.1）。本项目用强类型 NodeId 包装该标识，避免与普通字符串混淆，
// 并支持有序容器（CrossNodeIndex 用 std::set<NodeId>）与无序容器（见文末 std::hash）。
struct NodeId {
  std::string value;

  bool operator==(const NodeId& other) const = default;
  auto operator<=>(const NodeId& other) const = default;
};

// ---------------------------------------------------------------------------
// 数据类型 DType（KV 张量元素类型）
// ---------------------------------------------------------------------------
// 结构化枚举，禁止以字符串表示。底层定宽，便于序列化进定长头部（需求 3.5）。
enum class DType : std::uint16_t {
  kFloat16 = 0,    // IEEE half (fp16)
  kBFloat16 = 1,   // bfloat16
  kFloat8E4M3 = 2, // fp8 (e4m3)
  kFloat8E5M2 = 3, // fp8 (e5m2)
  kFloat32 = 4,    // fp32
  kInt8 = 5,       // int8（量化）
  kUInt8 = 6,      // uint8（量化）
};

// 返回某 DType 单元素占用的字节数。未知枚举返回 0（由调用方按错误处理）。
std::size_t DTypeSizeBytes(DType dtype) noexcept;

// 返回 DType 的稳定短名，供日志/诊断使用。
const char* DTypeName(DType dtype) noexcept;

// ---------------------------------------------------------------------------
// 块键 BlockKey
// ---------------------------------------------------------------------------
// KVCache_Block 的内容寻址键。hash_id 对应 FAST25 trace 的 hash_ids，
// layer 区分同一 token 块在各 Transformer 层的独立对象，version 为 schema 版本。
struct BlockKey {
  std::uint64_t hash_id = 0;                    // 内容哈希
  std::uint32_t layer = 0;                      // Transformer 层号
  std::uint16_t version = kBlockSchemaVersion;  // 序列化/schema 版本

  // 结构相等与全序（支撑有序/无序容器与测试断言）。
  bool operator==(const BlockKey& other) const = default;
  auto operator<=>(const BlockKey& other) const = default;
};

// ---------------------------------------------------------------------------
// KV 缓存块 KVCacheBlock
// ---------------------------------------------------------------------------
// 固定粒度的 K/V 缓存单元。k_payload / v_payload 为连续的 K、V 张量字节，
// 形状由 (num_tokens, num_heads, head_dim, dtype) 描述。
struct KVCacheBlock {
  BlockKey key;                          // 块键
  std::uint32_t num_tokens = 0;          // 本块覆盖的 token 数
  std::uint16_t num_heads = 0;           // 注意力头数
  std::uint16_t head_dim = 0;            // 单头维度
  DType dtype = DType::kFloat16;         // 元素类型
  std::vector<std::byte> k_payload;      // 连续 K 张量字节
  std::vector<std::byte> v_payload;      // 连续 V 张量字节

  // 结构相等：所有字段（含 payload 字节）逐一相等，用于序列化往返校验（需求 3.5）。
  bool operator==(const KVCacheBlock& other) const = default;
};

// ---------------------------------------------------------------------------
// 序列化形态 SerializedBlock
// ---------------------------------------------------------------------------
// Data_Path::Serialize 产出的线缆/存储形态：
//   [header | shape/dtype | K | V]，定长前缀、带版本（详见 datapath 任务 6.1）。
struct SerializedBlock {
  std::vector<std::byte> bytes;

  bool operator==(const SerializedBlock& other) const = default;
};

}  // namespace project

// BlockKey / NodeId 的 std::hash 特化，使其可用于 std::unordered_map/set
// （CrossNodeIndex 等控制面结构按需选用有序或无序容器）。
namespace std {

template <>
struct hash<project::BlockKey> {
  std::size_t operator()(const project::BlockKey& key) const noexcept {
    // 组合三字段哈希：以 64 位 hash_id 为主，混入 layer 与 version。
    std::size_t h = std::hash<std::uint64_t>{}(key.hash_id);
    h ^= std::hash<std::uint32_t>{}(key.layer) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    h ^= std::hash<std::uint16_t>{}(key.version) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    return h;
  }
};

template <>
struct hash<project::NodeId> {
  std::size_t operator()(const project::NodeId& node) const noexcept {
    return std::hash<std::string>{}(node.value);
  }
};

}  // namespace std

#endif  // PROJECT_TYPES_H_
