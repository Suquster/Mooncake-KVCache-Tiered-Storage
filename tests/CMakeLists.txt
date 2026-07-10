# =============================================================================
# tests/CMakeLists.txt —— C++ 测试目标（CTest + 可选 RapidCheck 属性测试）
# =============================================================================
# 约定：属性测试用 RapidCheck 编写，每个属性最少 100 次迭代，标签格式：
#   Feature: mooncake-kvcache-optimization, Property {n}: {text}
# 为保证在无网络受限环境也能完成核心构建与冒烟验证，RapidCheck 经
# FetchContent 拉取且默认关闭（ENABLE_RAPIDCHECK）；基础冒烟测试不依赖外部库。
# =============================================================================

# --- C++ 核心冒烟测试（无外部依赖，始终可构建）-------------------------------
add_executable(cpp_smoke_test cpp/smoke_test.cpp)
target_link_libraries(cpp_smoke_test PRIVATE project_core)
target_compile_options(cpp_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cpp_smoke_test COMMAND cpp_smoke_test)

# --- 核心数据模型与适配接口单元测试（任务 3.1，无外部依赖）--------------------
add_executable(cpp_data_models_test cpp/data_models_test.cpp)
target_link_libraries(cpp_data_models_test PRIVATE project_core)
target_compile_options(cpp_data_models_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cpp_data_models_test COMMAND cpp_data_models_test)

# --- 适配层接线单元测试（任务 3.4，内存态替身，无外部依赖）--------------------
add_executable(cpp_adapter_fake_test cpp/adapter_fake_test.cpp)
target_link_libraries(cpp_adapter_fake_test PRIVATE project_core)
target_compile_options(cpp_adapter_fake_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cpp_adapter_fake_test COMMAND cpp_adapter_fake_test)

# --- 分片并发存储：正确性测试 + 可选微基准（./cpp_sharded_store_test --bench）--
add_executable(cpp_sharded_store_test cpp/sharded_store_test.cpp)
target_link_libraries(cpp_sharded_store_test PRIVATE project_core)
target_compile_options(cpp_sharded_store_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cpp_sharded_store_test COMMAND cpp_sharded_store_test)

# --- 前缀预取器并发压测（TSan 数据竞争覆盖，无外部依赖）----------------------
add_executable(cpp_prefetcher_stress_test cpp/prefetcher_stress_test.cpp)
target_link_libraries(cpp_prefetcher_stress_test PRIVATE project_core)
target_compile_options(cpp_prefetcher_stress_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cpp_prefetcher_stress_test COMMAND cpp_prefetcher_stress_test)

# --- 可选：RapidCheck 属性测试harness ---------------------------------------
if(ENABLE_RAPIDCHECK)
  include(FetchContent)
  FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        master)
  FetchContent_MakeAvailable(rapidcheck)

  # 各组件属性测试目标（Properties 1–13、17、18，最少 100 次迭代/属性）。
  foreach(_prop_test IN ITEMS
      property_harness_test
      storage_property_test
      datapath_property_test
      scheduler_property_test)
    add_executable(cpp_${_prop_test} cpp/${_prop_test}.cpp)
    target_link_libraries(cpp_${_prop_test} PRIVATE project_core rapidcheck)
    target_compile_options(cpp_${_prop_test} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME cpp_${_prop_test} COMMAND cpp_${_prop_test})
  endforeach()
  message(STATUS "已启用 RapidCheck C++ 属性测试（storage/datapath/scheduler）")
else()
  message(STATUS "未启用 RapidCheck（ENABLE_RAPIDCHECK=OFF）；仅构建 C++ 冒烟测试")
endif()
