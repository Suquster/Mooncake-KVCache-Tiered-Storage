// 项目核心基础设施公共头文件。
// 暴露编译期固化的项目版本与「锁定的上游 Mooncake 版本」（需求 1.3 / 6.3），
// 供适配层、各组件以及 Python 绑定在运行时查询，保证依赖溯源一致。
#ifndef PROJECT_CORE_H_
#define PROJECT_CORE_H_

namespace project {

// 返回本项目语义化版本号（如 "0.1.0"）。
const char* ProjectVersion();

// 返回 mooncake.lock 锁定的上游 Mooncake 版本号（如 "0.3.6.post1"）。
const char* MooncakeRequiredVersion();

// 返回 mooncake.lock 锁定的上游 Mooncake commit 哈希。
const char* MooncakeRequiredCommit();

}  // namespace project

#endif  // PROJECT_CORE_H_
