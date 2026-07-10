# 合规声明（任务 15）

## 许可证

- 本仓库自研代码以 **Apache-2.0** 许可发布（见根目录 `LICENSE`），与上游
  [Mooncake](https://github.com/kvcache-ai/Mooncake)（Apache-2.0）兼容。
- 上游 Mooncake 以 **只读 git submodule** 形式引入（`third_party/mooncake`），
  未做任何在树修改（需求 6.2）；全部交互经适配层
  （`src/adapter/transfer_engine_adapter.h`、`src/adapter/store_adapter.h`）完成。

## 上游版本锁定与可复现性

- 锁定文件：根目录 `mooncake.lock`，记录上游 commit
  （完整 40 位哈希）与版本号（需求 6.3 / 1.3）。
- 子模块完整性由 `tests/test_submodule_smoke.py` 在 CI 中强制校验：
  子模块 HEAD 必须等于锁定 commit。
- 基准报告（`python/bench/framework.py`）在每次运行中回写锁定 commit、
  运行配置与软件版本，保证结果可溯源（需求 5.5）与可复现（需求 5.6）。

## 数据与第三方资产

- 基准回放使用公开的 FAST'25 Mooncake trace 格式
  （`mooncake_trace.jsonl`，字段：timestamp / input_length / output_length /
  hash_ids）；仓库不内嵌 trace 数据文件，运行时由使用者提供路径。
- 未引入任何闭源依赖；C++ 测试依赖 RapidCheck（BSD-2-Clause，
  经 CMake FetchContent 按需拉取），Python 测试依赖 pytest（MIT）与
  Hypothesis（MPL-2.0），均与 Apache-2.0 分发兼容。

## 安全与并发门禁

- CI（`.github/workflows/ci.yml`）在 AddressSanitizer+UBSan 与
  ThreadSanitizer 两种消毒器配置下重复运行全部 C++ 测试（需求 7.x 回归关卡）。
