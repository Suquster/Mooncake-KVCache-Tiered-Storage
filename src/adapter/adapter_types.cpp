// =============================================================================
// src/adapter/adapter_types.cpp —— 适配层数据模型的非内联实现
// =============================================================================
// 仅实现适配层接口头文件中声明的、按枚举分派的小工具函数（如 TransportPathName）。
// 这些函数与「项目自有」数据模型（TransportPath 等）绑定，属于任务 3.1 的接口定义范畴；
// 集中在此实现可避免头文件内联展开造成的重复定义风险，并给出权威的稳定短名来源
// （禁止散落魔数 / 字符串字面量重复）。
//
// 注意：本文件**不包含任何上游 Mooncake 头文件**——遵守「适配器边界」约定，
// 上游头文件仅由具体后端实现（任务 3.2 的 transfer_engine_adapter.cpp）include。
// =============================================================================
#include "adapter/transfer_engine_adapter.h"

namespace project::adapter {

const char* TransportPathName(TransportPath path) noexcept {
  switch (path) {
    case TransportPath::kRdma:
      return "rdma";
    case TransportPath::kGpuDirect:
      return "gpudirect";
    case TransportPath::kTcp:
      return "tcp";
  }
  return "unknown";  // 防御性兜底（理论不可达）
}

}  // namespace project::adapter
