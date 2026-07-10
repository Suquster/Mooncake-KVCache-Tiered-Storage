// RapidCheck 属性测试 harness（脚手架阶段的最小可工作示例）。
// 仅当 ENABLE_RAPIDCHECK=ON 时构建。后续各组件的属性测试（Properties 1–18）
// 将以相同模式加入，每个属性最少 100 次迭代，标签格式：
//   Feature: mooncake-kvcache-optimization, Property {n}: {text}
#include <rapidcheck.h>

#include <string>

#include "project/core.h"

int main() {
  bool ok = true;

  // 示例属性：对任意整数 n，版本/依赖溯源接口返回值恒为非空且稳定（幂等查询）。
  // 该属性确认 harness 接线正确（RapidCheck 默认运行 100 个样例）。
  ok &= rc::check(
      "Feature: mooncake-kvcache-optimization, Property 0: "
      "version provenance accessors are stable and non-empty",
      [](int /*ignored*/) {
        const std::string v1 = project::MooncakeRequiredVersion();
        const std::string v2 = project::MooncakeRequiredVersion();
        RC_ASSERT(!v1.empty());
        RC_ASSERT(v1 == v2);
      });

  return ok ? 0 : 1;
}
