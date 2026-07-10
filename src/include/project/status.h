// =============================================================================
// project/status.h —— 项目自有的错误模型 Status / Result<T>
// =============================================================================
// 设计依据（design.md「Error Handling」）：所有 C++ 核心 API 返回 Result<T> /
// Status，**不跨适配器边界抛异常**。适配器层把上游 Mooncake 的
// tl::expected<T, ErrorCode> / mooncake::Status 在边界处归一化为本类型
// （见 docs/analysis/upstream-mooncake-analysis.md §8.3 错误码映射表）。
//
// 约定：
//   * Status 表示「无返回值操作」的成败；Result<T> 表示「带返回值操作」的成败。
//   * 二者均为值语义（可拷贝/可移动），析构平凡，适配 RAII 与无异常错误传播。
//   * StatusCode 为结构化枚举（禁止用字符串表示错误类别），message 仅作诊断补充。
// =============================================================================
#ifndef PROJECT_STATUS_H_
#define PROJECT_STATUS_H_

#include <string>
#include <utility>
#include <variant>

namespace project {

// 结构化错误码。数值稳定（便于日志/跨语言映射），新增只追加、不重排。
// 各码与上游 Mooncake ErrorCode 的映射见分析文档 §8.3。
enum class StatusCode : int {
  kOk = 0,                  // 成功
  kInvalidArgument = 1,     // 参数非法（对应上游 kInvalidArgument / INVALID_*）
  kNotFound = 2,            // 对象/块不存在（对应 OBJECT_NOT_FOUND，缓存未命中 需求 2.5）
  kAlreadyExists = 3,       // 对象已存在（对应 OBJECT_ALREADY_EXISTS，写幂等去重）
  kOutOfCapacity = 4,       // 容量不足，需淘汰/降级（对应 NO_AVAILABLE_HANDLE 需求 2.4）
  kBackpressure = 5,        // 背压：提交队列已满（对应 kTooManyRequests/kBatchBusy 需求 3.4）
  kAddressNotRegistered = 6,// 源地址未注册，零拷贝前提不满足（对应 kAddressNotRegistered 需求 3.3）
  kTransportUnavailable = 7,// 选定传输后端不可用，需回退（对应 kNotSupportedTransport 需求 3.7）
  kTransferFailed = 8,      // 数据传输失败（对应 TRANSFER_FAIL，封装失败块键 需求 3.6）
  kUnreachable = 9,         // 节点失联，需故障切换（对应 RPC_FAIL 需求 4.5）
  kNotSupported = 10,       // 能力不支持（对应 kNotImplemented）
  kInternal = 11,           // 内部不变量被破坏（兜底）
};

// 返回错误码对应的稳定短名，供日志与诊断使用（非业务分支依据）。
const char* StatusCodeName(StatusCode code) noexcept;

// 操作成败的值类型。默认构造为成功（kOk）。
class Status {
 public:
  // 默认构造：成功。
  Status() noexcept : code_(StatusCode::kOk) {}

  // 由错误码 + 可选诊断信息构造。code==kOk 时 message 被忽略以保持「成功无消息」语义。
  Status(StatusCode code, std::string message)
      : code_(code),
        message_(code == StatusCode::kOk ? std::string{} : std::move(message)) {}

  explicit Status(StatusCode code) : Status(code, std::string{}) {}

  // 便捷构造器。
  static Status Ok() noexcept { return Status{}; }
  static Status Make(StatusCode code, std::string message) {
    return Status{code, std::move(message)};
  }

  bool ok() const noexcept { return code_ == StatusCode::kOk; }
  StatusCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  // 结构相等：错误码与诊断信息均相等（便于测试断言）。
  bool operator==(const Status& other) const = default;

  // 人读字符串，形如 "kNotFound: <message>" 或成功时 "kOk"。
  std::string ToString() const;

 private:
  StatusCode code_;
  std::string message_;
};

// 带返回值操作的成败包装：要么持有成功值 T，要么持有非 kOk 的 Status。
// 语义参考 absl::StatusOr：调用方先 ok() 再取 value()。不主动抛异常作错误信号。
template <typename T>
class Result {
 public:
  // 由成功值构造（拷贝/移动）。
  Result(const T& value) : payload_(value) {}              // NOLINT(runtime/explicit)
  Result(T&& value) : payload_(std::move(value)) {}        // NOLINT(runtime/explicit)

  // 由错误状态构造；若误传 kOk，归一化为 kInternal 以维持「错误分支必为非 kOk」不变量。
  Result(Status status)                                    // NOLINT(runtime/explicit)
      : payload_(status.ok()
                     ? Status{StatusCode::kInternal,
                              "Result 由 kOk 的 Status 构造，违反不变量"}
                     : std::move(status)) {}

  bool ok() const noexcept { return std::holds_alternative<T>(payload_); }
  explicit operator bool() const noexcept { return ok(); }

  // 错误状态：ok() 时返回 kOk 的 Status。
  Status status() const {
    if (ok()) return Status::Ok();
    return std::get<Status>(payload_);
  }

  // 成功值访问。前置条件：ok()==true。提供安全的指针式与默认值式访问以避免误用。
  const T* try_value() const noexcept { return std::get_if<T>(&payload_); }
  T* try_value() noexcept { return std::get_if<T>(&payload_); }

  // 取成功值（前置条件 ok()）；契约违例时由 std::variant 以 std::bad_variant_access
  // 报告（属编程错误路径，非正常错误信号——错误信号一律经 Status 传播）。
  const T& value() const& { return std::get<T>(payload_); }
  T& value() & { return std::get<T>(payload_); }
  T&& value() && { return std::get<T>(std::move(payload_)); }

  // 失败时返回回退值，永不触发契约违例。
  T value_or(T fallback) const {
    if (const T* v = try_value()) return *v;
    return fallback;
  }

 private:
  std::variant<T, Status> payload_;
};

}  // namespace project

#endif  // PROJECT_STATUS_H_
