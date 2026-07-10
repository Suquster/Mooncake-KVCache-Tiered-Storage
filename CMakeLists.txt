# =============================================================================
# Mooncake KVCache 优化项目 —— C++ 核心构建入口
# =============================================================================
# 本项目作为「独立仓库」分层于上游 Mooncake（kvcache-ai/Mooncake）之上，
# 通过 src/adapter/* 适配层消费 Mooncake 子模块（需求 6.2）。
# 构建配置阶段会校验上游版本是否与 mooncake.lock 锁定一致（需求 6.3 / 6.5）。
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# 读取项目版本（与 pyproject.toml 保持一致，集中管理避免魔数散落）。
set(PROJECT_VERSION_STRING "0.1.0")

project(mooncake_kvcache_optimization
        VERSION ${PROJECT_VERSION_STRING}
        DESCRIPTION "基于上游 Mooncake 的分层 KVCache 优化项目（CCF 2026 赛题二）"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- 全局编译标准与告警 -------------------------------------------------------
# 采用 C++20：核心数据模型（design.md「Data Models」）使用 defaulted operator== 与
# 三路比较 operator<=>（BlockKey/NodeId 全序、结构相等），这些是 C++20 特性。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "构建类型" FORCE)
endif()

# --- 可选特性开关 -------------------------------------------------------------
# 默认关闭需要外部依赖（pybind11 / RapidCheck）的目标，以便在无网络的
# 受限环境中仍可完成核心构建与冒烟验证；CI 中按需开启。
option(BUILD_PYTHON_BINDINGS "构建 pybind11 Python 绑定（需要 pybind11 与 Python 开发头文件）" OFF)
option(ENABLE_RAPIDCHECK     "启用基于 RapidCheck 的 C++ 属性测试（需要 RapidCheck）" OFF)
option(BUILD_TESTING_PROJECT "构建本项目的测试目标（CTest）" ON)
option(WITH_MOONCAKE_TE      "编译上游 Mooncake Transfer Engine 生产后端（编译期 ABI 门禁）" OFF)

# --- 上游 Mooncake 版本构建期校验（需求 6.3 / 6.5）---------------------------
# 缺失或版本不一致时，include 内部会以「指明所需 Mooncake 版本」的 FATAL_ERROR 失败。
include(CheckMooncakeVersion)

# --- 生成版本头文件（把锁定的上游版本固化进 C++ 编译期常量）------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/project_version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/project/version.h"
  @ONLY)

# --- 核心子目录 ---------------------------------------------------------------
add_subdirectory(src)

if(BUILD_TESTING_PROJECT)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "项目 ${PROJECT_NAME} v${PROJECT_VERSION} 配置完成；"
               "所需上游 Mooncake 版本：${MOONCAKE_REQUIRED_VERSION}")
