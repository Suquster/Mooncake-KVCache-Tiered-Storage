# =============================================================================
# src/CMakeLists.txt —— C++ 核心库与（可选）Python 绑定
# =============================================================================
# 分层约定（详见 design.md）：
#   adapter/   仅此目录可包含上游 Mooncake 头文件 / 链接其库（Project↔Mooncake 边界）
#   storage/   分层存储管理器、预取、淘汰策略
#   datapath/  序列化、零拷贝、异步 I/O 流水线、传输编排
#   scheduler/ 跨节点索引、公平调度、负载均衡、故障转移
#   core/      跨层共享的基础设施（版本信息等）
# 后续任务将逐步在上述子目录补充实现；本任务仅搭建可编译的脚手架。
# =============================================================================

# --- 核心基础设施库（编译期版本信息 + 核心数据模型 + 适配层接口工具）-----------
# 约定：
#   core/version.cpp      编译期固化的项目/上游版本（需求 1.3）
#   core/types.cpp        Status/DType 等枚举短名与 DType 字节宽度（数据模型，任务 3.1）
#   adapter/adapter_types.cpp  适配层接口的枚举短名实现（TransportPath，任务 3.1）
# 适配层与各组件头文件以 "adapter/..."、"project/..." 形式包含，故把 src/ 与 src/include
# 同时纳入 PUBLIC 包含路径，使依赖 project_core 的目标无需重复声明。
add_library(project_core STATIC
  core/version.cpp
  core/types.cpp
  adapter/adapter_types.cpp
  storage/tiered_storage_manager.cpp
  storage/prefix_prefetcher.cpp
  datapath/data_path.cpp
  scheduler/scheduler.cpp)

find_package(Threads REQUIRED)
target_link_libraries(project_core PUBLIC Threads::Threads)

target_include_directories(project_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated")

target_compile_options(project_core PRIVATE
  -Wall -Wextra -Wpedantic)

# --- 可选：上游 Mooncake Transfer Engine 生产后端（适配器边界的唯一上游接线）--
# WITH_MOONCAKE_TE=ON 时编译 mooncake_transfer_backend（对上游 C ABI 的实现）。
# 编译仅需上游头文件（随子模块提供）；最终可执行程序链接时需提供
# MOONCAKE_TE_LIBRARY（上游预编译 libtransfer_engine）。CI 以 OBJECT 库形式
# 编译门禁本文件，确保与锁定的上游 ABI 持续兼容（需求 6.2/6.3）。
if(WITH_MOONCAKE_TE)
  add_library(project_mooncake_backend OBJECT adapter/mooncake_transfer_backend.cpp)
  target_include_directories(project_mooncake_backend PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/third_party/mooncake/mooncake-transfer-engine/include")
  target_compile_options(project_mooncake_backend PRIVATE -Wall -Wextra -Wpedantic)
  message(STATUS "已启用上游 Transfer Engine 后端编译（WITH_MOONCAKE_TE=ON）")
endif()

# --- 可选：pybind11 Python 绑定（语言边界，需求中的「绑定桩」）----------------
# Python 编排层（vLLM 适配、基准框架）通过该绑定调用 C++ 核心。
if(BUILD_PYTHON_BINDINGS)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_mooncake_kvcache bindings/bindings.cpp)
  target_link_libraries(_mooncake_kvcache PRIVATE project_core)
  message(STATUS "已启用 pybind11 绑定目标 _mooncake_kvcache")
else()
  message(STATUS "未启用 Python 绑定（BUILD_PYTHON_BINDINGS=OFF）")
endif()
