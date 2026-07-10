---
inclusion: always
---

# 生产级实现标准（Production-Grade Implementation Standards）

本规则定义"生产级"的具体含义。任何声称"已完成"的实现必须满足以下全部标准，否则不视为完成。

---

## 一、"生产级"的定义（必须全部满足）

一个模块达到"生产级"意味着：

1. **可直接集成到真实应用中使用**，而不仅仅是"能跑通示例"
2. **性能在合理范围内** — 无 O(n²) 热路径、无不必要的内存分配、关键路径有基准测试
3. **错误处理完备** — 不会因为意外输入 panic，所有错误路径有明确的返回值或诊断信息
4. **API 设计成熟** — 命名一致、职责单一、易于组合、文档完整
5. **边界条件覆盖** — 空输入、超大输入、畸形输入、并发访问（如适用）都有定义行为

---

## 二、各方向"生产级"的具体要求

### regex-engine
- ❌ 不合格：只支持 ASCII 大小写折叠
- ✅ 合格：支持 Unicode General Category，惰性 DFA 有缓存淘汰，有 `is_match` 快速路径
- 对标：至少达到 Go `regexp` 库的功能覆盖度

### parser-combinator
- ❌ 不合格：流式输入每次从头重解析
- ✅ 合格：基于续延的增量解析，有错误恢复机制，packrat 有内存控制
- 对标：至少达到 Haskell `parsec` 的核心功能

### serialization
- ❌ 不合格：只支持基本类型编解码，代码生成用字符串拼接
- ✅ 合格：支持 service/rpc/import，结构化 AST 代码生成，有流式接口
- 对标：至少达到 `prost`（Rust protobuf）的核心功能

### logging
- ❌ 不合格：Sink 只写内存 Array
- ✅ 合格：有 ConsoleSink/CallbackSink/BufferedSink，有运行时调级
- 对标：至少达到 Go `slog` 的核心架构

### build-tool
- ❌ 不合格：只有依赖图建模，不能执行任何动作
- ✅ 合格：有 Action/Executor 框架，有并行调度，有增量构建日志
- 对标：至少达到 Haskell `Shake` 的调度模型

### codegen-infra
- ❌ 不合格：IR 用字符串表示指令
- ✅ 合格：类型化 IR 枚举，有验证器和解释器
- 对标：至少达到教学编译器（如 Cornell CS 4120）的 IR 设计水平

### dst
- ❌ 不合格：Task 只是 {id, name}
- ✅ 合格：可执行 TaskBody，有模拟网络/时钟/不变量检查
- 对标：至少达到 FoundationDB 仿真器的概念模型

### mini-compiler
- ❌ 不合格：只有基本的 HM 推断和简单 VM
- ✅ 合格：有模式匹配/元组/列表，有 peephole/TCO 优化，有精确类型错误
- 对标：至少达到 OCaml 入门子集的表达力

### lsp
- ❌ 不合格：JSON-RPC 不完整，增量同步可能 O(n²)
- ✅ 合格：JSON-RPC 2.0 完整，多换行符兼容，增量同步 O(n)
- 对标：至少达到 LSP 规范 3.17 的基础功能集

### moonbit-infra-suite (PBT)
- ❌ 不合格：没有 shrink，没有生成器组合子
- ✅ 合格：有 shrink 收缩反例，有 one_of/frequency/sized，有统计收集
- 对标：至少达到 Haskell `QuickCheck` 的核心功能

---

## 三、完成检查清单（每个方向必须逐项确认）

完成一个方向的加固后，必须确认以下所有项：

- [ ] 无 O(n²) 字符串拼接（grep 确认循环内无 `out = out +` / `result = result +`）
- [ ] 无 `abort()` / `todo!()` / `unimplemented` 占位
- [ ] 无字符串模拟结构化数据
- [ ] 所有新增公开函数有属性测试（≥100 迭代）
- [ ] `moon info` 后 `.mbti` 只增不减
- [ ] `moon fmt` 无格式化差异
- [ ] `moon test` 全部通过（wasm-gc）
- [ ] `moon test --target js` 已完成部分全通过
- [ ] `moon test --target native` 已完成部分全通过
- [ ] 现有测试未被修改
- [ ] README.mbt.md 已更新
- [ ] CHANGELOG.md 已更新
