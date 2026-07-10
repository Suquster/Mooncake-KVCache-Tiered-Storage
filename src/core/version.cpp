// 项目核心版本信息实现。
// 实际常量来自 CMake 在配置阶段生成的 project/version.h（其值取自 mooncake.lock），
// 此处仅做转发，确保「锁定版本」在整个 C++ 代码库中只有一个可信来源。
#include "project/core.h"

#include "project/version.h"  // 由 CMake configure_file 生成

namespace project {

const char* ProjectVersion() { return kProjectVersion; }

const char* MooncakeRequiredVersion() { return kMooncakeRequiredVersion; }

const char* MooncakeRequiredCommit() { return kMooncakeRequiredCommit; }

}  // namespace project
