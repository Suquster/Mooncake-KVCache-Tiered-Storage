// C++ 核心冒烟测试：验证项目核心库可链接、版本/依赖溯源接口返回非空且自洽。
// 不依赖任何外部测试框架，保证在受限环境中始终可构建运行（CTest）。
#include <cstdio>
#include <cstring>

#include "project/core.h"

namespace {

// 简易断言：失败时打印信息并返回非零退出码（避免引入第三方框架）。
int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "[失败] %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  const char* version = project::ProjectVersion();
  const char* mc_version = project::MooncakeRequiredVersion();
  const char* mc_commit = project::MooncakeRequiredCommit();

  Check(version != nullptr && std::strlen(version) > 0,
        "ProjectVersion 应返回非空字符串");
  Check(mc_version != nullptr && std::strlen(mc_version) > 0,
        "MooncakeRequiredVersion 应返回非空字符串");
  Check(mc_commit != nullptr && std::strlen(mc_commit) == 40,
        "MooncakeRequiredCommit 应为 40 位完整 commit 哈希");

  if (g_failures == 0) {
    std::printf("[通过] C++ 冒烟测试：项目 v%s，锁定 Mooncake v%s（commit %s）\n",
                version, mc_version, mc_commit);
  }
  return g_failures == 0 ? 0 : 1;
}
