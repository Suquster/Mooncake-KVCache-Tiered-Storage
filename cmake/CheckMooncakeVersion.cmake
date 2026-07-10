# =============================================================================
# CheckMooncakeVersion.cmake —— 上游 Mooncake 版本构建期校验（需求 6.3 / 6.5）
# =============================================================================
#
# 设计目标：
#   构建配置阶段校验 third_party/mooncake 子模块的实际版本，是否与 mooncake.lock
#   中锁定的版本一致。当子模块缺失、未初始化或版本不匹配时，必须以「明确指明所需
#   Mooncake 版本」的致命错误使构建失败（需求 6.5）。
#
# 双模式可用：
#   1) 由根 CMakeLists.txt 通过 include() 引入，在正常配置流程中执行校验。
#   2) 通过脚本模式 `cmake -P CheckMooncakeVersion.cmake` 独立运行，供冒烟测试调用，
#      以验证「版本缺失时构建失败并指明所需版本」这一行为（任务 1.1）。
#
# 可覆盖变量（脚本模式下通过 -D 传入，须置于 -P 之前）：
#   REPO_ROOT                仓库根目录，默认推断为本文件上一级目录。
#   MOONCAKE_LOCK_FILE       锁定文件路径，默认 ${REPO_ROOT}/mooncake.lock。
#   MOONCAKE_SUBMODULE_DIR   子模块目录，默认 ${REPO_ROOT}/third_party/mooncake。
#
# 输出变量（include 模式下回传给父作用域）：
#   MOONCAKE_REQUIRED_VERSION / MOONCAKE_REQUIRED_COMMIT / MOONCAKE_ACTUAL_VERSION
# =============================================================================

# --- 解析默认路径 -------------------------------------------------------------
if(NOT DEFINED REPO_ROOT)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED MOONCAKE_LOCK_FILE)
  set(MOONCAKE_LOCK_FILE "${REPO_ROOT}/mooncake.lock")
endif()
if(NOT DEFINED MOONCAKE_SUBMODULE_DIR)
  set(MOONCAKE_SUBMODULE_DIR "${REPO_ROOT}/third_party/mooncake")
endif()

# 上游版本号在其 wheel 工程中的权威位置（相对于子模块根目录）。
set(_MOONCAKE_VERSION_FILE_REL "mooncake-wheel/pyproject.toml")

# --- 工具函数：从 mooncake.lock 读取某个 KEY 的值 ----------------------------
function(_mooncake_lock_get _key _out_var)
  set(${_out_var} "" PARENT_SCOPE)
  if(NOT EXISTS "${MOONCAKE_LOCK_FILE}")
    return()
  endif()
  file(STRINGS "${MOONCAKE_LOCK_FILE}" _lines)
  foreach(_line IN LISTS _lines)
    # 跳过注释与空行。
    if(_line MATCHES "^[ \t]*#" OR _line STREQUAL "")
      continue()
    endif()
    if(_line MATCHES "^[ \t]*${_key}[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" _val)
      set(${_out_var} "${_val}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

# --- 1. 读取锁定的所需版本与 commit ------------------------------------------
if(NOT EXISTS "${MOONCAKE_LOCK_FILE}")
  message(FATAL_ERROR
    "[Mooncake 依赖校验] 缺少锁定文件 mooncake.lock（期望路径：${MOONCAKE_LOCK_FILE}）。"
    " 该文件必须记录所需的 Mooncake commit 哈希与版本号。")
endif()

_mooncake_lock_get("mooncake_version" MOONCAKE_REQUIRED_VERSION)
_mooncake_lock_get("mooncake_commit"  MOONCAKE_REQUIRED_COMMIT)
_mooncake_lock_get("mooncake_tag"     MOONCAKE_REQUIRED_TAG)

if(MOONCAKE_REQUIRED_VERSION STREQUAL "" OR MOONCAKE_REQUIRED_COMMIT STREQUAL "")
  message(FATAL_ERROR
    "[Mooncake 依赖校验] mooncake.lock 未同时记录 mooncake_version 与 mooncake_commit；"
    " 无法确定所需的 Mooncake 版本。")
endif()

# --- 2. 读取子模块的实际版本 --------------------------------------------------
set(_version_file "${MOONCAKE_SUBMODULE_DIR}/${_MOONCAKE_VERSION_FILE_REL}")
set(MOONCAKE_ACTUAL_VERSION "")
if(EXISTS "${_version_file}")
  file(STRINGS "${_version_file}" _ver_lines REGEX "^[ \t]*version[ \t]*=")
  foreach(_vline IN LISTS _ver_lines)
    if(_vline MATCHES "version[ \t]*=[ \t]*[\"']([^\"']+)[\"']")
      set(MOONCAKE_ACTUAL_VERSION "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()
endif()

# --- 3. 校验：缺失或不一致则以「指明所需版本」的错误使构建失败（需求 6.5）----
if(NOT EXISTS "${MOONCAKE_SUBMODULE_DIR}/.git" AND NOT EXISTS "${_version_file}")
  message(FATAL_ERROR
    "[Mooncake 依赖校验] 未找到上游 Mooncake 子模块（目录：${MOONCAKE_SUBMODULE_DIR}）。"
    " 本项目需要 Mooncake 版本 ${MOONCAKE_REQUIRED_VERSION}"
    "（tag ${MOONCAKE_REQUIRED_TAG}，commit ${MOONCAKE_REQUIRED_COMMIT}）。"
    " 请运行：git submodule update --init --recursive third_party/mooncake")
endif()

if(MOONCAKE_ACTUAL_VERSION STREQUAL "")
  message(FATAL_ERROR
    "[Mooncake 依赖校验] 无法在 ${_version_file} 读取到上游 Mooncake 版本号。"
    " 本项目需要 Mooncake 版本 ${MOONCAKE_REQUIRED_VERSION}"
    "（commit ${MOONCAKE_REQUIRED_COMMIT}）。请检查子模块是否已检出到锁定的 commit。")
endif()

if(NOT MOONCAKE_ACTUAL_VERSION STREQUAL MOONCAKE_REQUIRED_VERSION)
  message(FATAL_ERROR
    "[Mooncake 依赖校验] 上游 Mooncake 版本不匹配："
    " 实际为 ${MOONCAKE_ACTUAL_VERSION}，但本项目需要 ${MOONCAKE_REQUIRED_VERSION}"
    "（commit ${MOONCAKE_REQUIRED_COMMIT}）。"
    " 请将子模块检出到锁定的 commit：git -C ${MOONCAKE_SUBMODULE_DIR} checkout ${MOONCAKE_REQUIRED_COMMIT}")
endif()

message(STATUS
  "[Mooncake 依赖校验] 通过：Mooncake ${MOONCAKE_ACTUAL_VERSION}"
  "（commit ${MOONCAKE_REQUIRED_COMMIT}）。")

# 说明：本文件以 include() 方式引入时与调用者共享同一作用域，
# 因此 MOONCAKE_REQUIRED_VERSION / MOONCAKE_REQUIRED_COMMIT / MOONCAKE_ACTUAL_VERSION
# 这些普通变量对调用者（根 CMakeLists.txt）直接可见，无需 PARENT_SCOPE 回传。
