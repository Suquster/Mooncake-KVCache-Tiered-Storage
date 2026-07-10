// 核心数据模型的非内联实现：StatusCode/DType 短名与 DType 字节宽度、Status::ToString。
// 集中实现这些「按枚举分派的小函数」，避免在头文件中内联展开造成的重复定义风险，
// 并为后续序列化（需求 3.5）提供权威的 DType 字节宽度来源（禁止散落魔数）。
#include "project/status.h"
#include "project/types.h"

#include <cstddef>
#include <string>

namespace project {

const char* StatusCodeName(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk:
      return "kOk";
    case StatusCode::kInvalidArgument:
      return "kInvalidArgument";
    case StatusCode::kNotFound:
      return "kNotFound";
    case StatusCode::kAlreadyExists:
      return "kAlreadyExists";
    case StatusCode::kOutOfCapacity:
      return "kOutOfCapacity";
    case StatusCode::kBackpressure:
      return "kBackpressure";
    case StatusCode::kAddressNotRegistered:
      return "kAddressNotRegistered";
    case StatusCode::kTransportUnavailable:
      return "kTransportUnavailable";
    case StatusCode::kTransferFailed:
      return "kTransferFailed";
    case StatusCode::kUnreachable:
      return "kUnreachable";
    case StatusCode::kNotSupported:
      return "kNotSupported";
    case StatusCode::kInternal:
      return "kInternal";
  }
  return "kUnknown";  // 防御性兜底（理论不可达）
}

std::string Status::ToString() const {
  std::string out = StatusCodeName(code_);
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

std::size_t DTypeSizeBytes(DType dtype) noexcept {
  switch (dtype) {
    case DType::kFloat16:
    case DType::kBFloat16:
      return 2;
    case DType::kFloat8E4M3:
    case DType::kFloat8E5M2:
    case DType::kInt8:
    case DType::kUInt8:
      return 1;
    case DType::kFloat32:
      return 4;
  }
  return 0;  // 未知类型：返回 0，由调用方按错误处理（如序列化拒绝）
}

const char* DTypeName(DType dtype) noexcept {
  switch (dtype) {
    case DType::kFloat16:
      return "fp16";
    case DType::kBFloat16:
      return "bf16";
    case DType::kFloat8E4M3:
      return "fp8_e4m3";
    case DType::kFloat8E5M2:
      return "fp8_e5m2";
    case DType::kFloat32:
      return "fp32";
    case DType::kInt8:
      return "int8";
    case DType::kUInt8:
      return "uint8";
  }
  return "unknown";  // 防御性兜底
}

}  // namespace project
