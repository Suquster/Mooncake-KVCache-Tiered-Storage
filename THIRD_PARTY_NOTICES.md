# 第三方来源、协议与依赖声明

按大赛核心规则（使用开源代码须声明来源、协议与依赖关系）列出本作品的全部
第三方依赖。本作品自身以 Apache License 2.0 开源（见 [LICENSE](LICENSE)）。

## 运行时/构建依赖

| 依赖 | 来源 | 协议 | 用途 |
|---|---|---|---|
| Mooncake（上游） | https://github.com/kvcache-ai/Mooncake （v0.3.6.post1，commit 锁定见 `mooncake.lock`） | Apache-2.0 | Transfer Engine C ABI 接线；FAST'25 trace 数据集（`third_party/mooncake/FAST25-release/`） |
| pybind11 | https://github.com/pybind/pybind11 | BSD-3-Clause | Python↔C++ 绑定 |
| vLLM | https://github.com/vllm-project/vllm | Apache-2.0 | KV connector 对接接口（仅接口适配，不捆绑分发） |

## 测试/基准依赖（不随产物分发）

| 依赖 | 来源 | 协议 | 用途 |
|---|---|---|---|
| RapidCheck | https://github.com/emil-e/rapidcheck | BSD-2-Clause | C++ 属性测试（`ENABLE_RAPIDCHECK=ON` 时 FetchContent 拉取） |
| Hypothesis | https://github.com/HypothesisWorks/hypothesis | MPL-2.0 | Python 属性测试 |
| pytest | https://github.com/pytest-dev/pytest | MIT | Python 测试驱动 |

## 数据集

- **FAST'25 Mooncake traces**（arxiv / conversation / synthetic / toolagent）：
  来自上游 Mooncake 仓库 `FAST25-release/` 目录（Apache-2.0），仅用于确定性
  回放基准，未做任何修改。

## 原创性声明

除上表所列第三方组件外，`src/`、`python/`、`tests/`、`docs/` 下的全部设计
与实现（分层存储管理器、S3-FIFO/纯 LRU 淘汰、前缀感知多锚点预取、分片并发
存储、基准框架等）均为本队原创。算法思想引用的公开文献：S3-FIFO
（Yang et al., SOSP'23）、SIEVE（Zhang et al., NSDI'24，经评估未采纳，见
docs/benchmarks/REPORT.md 负结果记录）。
