// 数据面实现（设计依据见头文件与 design.md「Data_Path」）。
#include "datapath/data_path.h"

#include <cstring>
#include <string>

namespace project::datapath {

namespace {

// 线缆头魔数（'M''K''V''B'）与序列化布局常量（需求 3.5）。
constexpr std::uint32_t kMagic = 0x424B564DU;  // little-endian "MKVB"
// 固定头部字节数：u64 总长 + u32 魔数 + u16 版本 + u16 dtype + u64 hash_id +
// u32 layer + u32 num_tokens + u16 heads + u16 head_dim + u64 k_len + u64 v_len。
constexpr std::size_t kHeaderBytes = 8 + 4 + 2 + 2 + 8 + 4 + 4 + 2 + 2 + 8 + 8;

// 小端序定宽整数写入/读取（跨平台稳定的线缆格式）。
template <typename T>
void AppendLE(std::vector<std::byte>& out, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
  }
}

template <typename T>
T ReadLE(const std::vector<std::byte>& in, std::size_t offset) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<std::uint8_t>(in[offset + i]))
             << (8 * i);
  }
  return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// ScopedRegistration
// ---------------------------------------------------------------------------
ScopedRegistration::ScopedRegistration(adapter::ITransferBackend& backend,
                                       void* addr, std::size_t len,
                                       bool on_gpu)
    : backend_(&backend) {
  auto result = backend.RegisterBuffer(addr, len, on_gpu);
  if (result.ok()) {
    handle_ = result.value();
    valid_ = true;
  } else {
    status_ = result.status();
  }
}

ScopedRegistration::~ScopedRegistration() { Release(); }

ScopedRegistration::ScopedRegistration(ScopedRegistration&& other) noexcept
    : backend_(other.backend_),
      handle_(other.handle_),
      status_(std::move(other.status_)),
      valid_(other.valid_) {
  other.valid_ = false;
  other.backend_ = nullptr;
}

ScopedRegistration& ScopedRegistration::operator=(
    ScopedRegistration&& other) noexcept {
  if (this != &other) {
    Release();
    backend_ = other.backend_;
    handle_ = other.handle_;
    status_ = std::move(other.status_);
    valid_ = other.valid_;
    other.valid_ = false;
    other.backend_ = nullptr;
  }
  return *this;
}

void ScopedRegistration::Release() noexcept {
  if (valid_ && backend_ != nullptr) {
    (void)backend_->DeregisterBuffer(handle_);
    valid_ = false;
  }
}

// ---------------------------------------------------------------------------
// 序列化（需求 3.5）
// ---------------------------------------------------------------------------
SerializedBlock DataPath::Serialize(const KVCacheBlock& block) {
  SerializedBlock out;
  const std::size_t total =
      kHeaderBytes + block.k_payload.size() + block.v_payload.size();
  out.bytes.reserve(total);
  AppendLE<std::uint64_t>(out.bytes, total);
  AppendLE<std::uint32_t>(out.bytes, kMagic);
  AppendLE<std::uint16_t>(out.bytes, block.key.version);
  AppendLE<std::uint16_t>(out.bytes, static_cast<std::uint16_t>(block.dtype));
  AppendLE<std::uint64_t>(out.bytes, block.key.hash_id);
  AppendLE<std::uint32_t>(out.bytes, block.key.layer);
  AppendLE<std::uint32_t>(out.bytes, block.num_tokens);
  AppendLE<std::uint16_t>(out.bytes, block.num_heads);
  AppendLE<std::uint16_t>(out.bytes, block.head_dim);
  AppendLE<std::uint64_t>(out.bytes, block.k_payload.size());
  AppendLE<std::uint64_t>(out.bytes, block.v_payload.size());
  out.bytes.insert(out.bytes.end(), block.k_payload.begin(),
                   block.k_payload.end());
  out.bytes.insert(out.bytes.end(), block.v_payload.begin(),
                   block.v_payload.end());
  return out;
}

Result<KVCacheBlock> DataPath::Deserialize(const SerializedBlock& serialized) {
  const auto& bytes = serialized.bytes;
  auto invalid = [](std::string reason) {
    return Result<KVCacheBlock>(
        Status::Make(StatusCode::kInvalidArgument, std::move(reason)));
  };
  if (bytes.size() < kHeaderBytes) {
    return invalid("序列化字节不足以容纳固定头部");
  }
  std::size_t off = 0;
  const auto total = ReadLE<std::uint64_t>(bytes, off);
  off += 8;
  if (total != bytes.size()) {
    return invalid("长度前缀与实际字节数不一致");
  }
  const auto magic = ReadLE<std::uint32_t>(bytes, off);
  off += 4;
  if (magic != kMagic) {
    return invalid("魔数不匹配（非本格式或已损坏）");
  }
  KVCacheBlock block;
  block.key.version = ReadLE<std::uint16_t>(bytes, off);
  off += 2;
  if (block.key.version != kBlockSchemaVersion) {
    return invalid("不支持的 schema 版本");
  }
  block.dtype = static_cast<DType>(ReadLE<std::uint16_t>(bytes, off));
  off += 2;
  block.key.hash_id = ReadLE<std::uint64_t>(bytes, off);
  off += 8;
  block.key.layer = ReadLE<std::uint32_t>(bytes, off);
  off += 4;
  block.num_tokens = ReadLE<std::uint32_t>(bytes, off);
  off += 4;
  block.num_heads = ReadLE<std::uint16_t>(bytes, off);
  off += 2;
  block.head_dim = ReadLE<std::uint16_t>(bytes, off);
  off += 2;
  const auto k_len = ReadLE<std::uint64_t>(bytes, off);
  off += 8;
  const auto v_len = ReadLE<std::uint64_t>(bytes, off);
  off += 8;
  if (kHeaderBytes + k_len + v_len != bytes.size()) {
    return invalid("K/V 负载长度与总长不一致");
  }
  block.k_payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                         bytes.begin() + static_cast<std::ptrdiff_t>(off + k_len));
  off += k_len;
  block.v_payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                         bytes.begin() + static_cast<std::ptrdiff_t>(off + v_len));
  return Result<KVCacheBlock>(std::move(block));
}

// ---------------------------------------------------------------------------
// 异步流水线（需求 3.4 / 3.6）
// ---------------------------------------------------------------------------
DataPath::DataPath(adapter::ITransferBackend& backend,
                   std::size_t queue_capacity)
    : backend_(backend),
      queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity),
      worker_([this] { WorkerLoop(); }) {}

DataPath::~DataPath() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    shutting_down_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  // 未及处理的排队请求以关停错误兑现，避免悬挂的 future。
  for (auto& item : queue_) {
    item.promise.set_value(Result<adapter::TransferReceipt>(Status::Make(
        StatusCode::kInternal, "数据面关停，请求未执行")));
  }
}

std::future<Result<adapter::TransferReceipt>> DataPath::TransferAsync(
    const adapter::TransferRequest& request) {
  std::promise<Result<adapter::TransferReceipt>> promise;
  auto future = promise.get_future();
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.size() >= queue_capacity_ || shutting_down_) {
      // 背压：队满即拒绝，错误携带失败块键（需求 3.4 / 3.6）。
      promise.set_value(Result<adapter::TransferReceipt>(Status::Make(
          StatusCode::kBackpressure,
          "提交队列已满，施加背压；failing_key.hash_id=" +
              std::to_string(request.key.hash_id))));
      return future;
    }
    adapter::TransferRequest routed = request;
    routed.preferred = backend_.SelectPath(request);
    queue_.push_back(PendingItem{routed, std::move(promise)});
  }
  cv_.notify_one();
  return future;
}

std::size_t DataPath::PendingDepth() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

void DataPath::WorkerLoop() {
  for (;;) {
    PendingItem item;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return shutting_down_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;  // 关停且无积压。
      }
      item = std::move(queue_.front());
      queue_.pop_front();
    }
    // 逐条转交后端异步提交；结果（成功回执或含失败键的错误）转发给调用方 future。
    auto backend_future = backend_.SubmitAsync(item.request);
    item.promise.set_value(backend_future.get());
  }
}

}  // namespace project::datapath
