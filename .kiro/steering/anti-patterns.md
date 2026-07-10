---
inclusion: always
---

# 禁止反模式清单（Anti-Pattern Blocklist）

以下反模式在本项目中**绝对禁止**。如果发现自己即将使用这些模式，必须停下来重新思考设计。

---

## 绝对禁止的代码反模式

### 1. 字符串模拟一切
```
// ❌ 绝对禁止
let instr = "ADD r1, r2, r3"
let target = TargetInstr { op: "MOV" }
let block = BasicBlock { instrs: Array["ADD", "SUB"] }

// ✅ 必须使用类型化表示
let instr = Add(Reg(1), Reg(2), Reg(3))
let target = Mov(dst=Reg(1), src=Reg(2))
```

### 2. O(n²) 循环拼接
```
// ❌ 绝对禁止
fn build_output(items) {
  let mut out = ""
  for item in items {
    out = out + item.to_string()  // 每次复制整个 out
  }
  out
}

// ✅ 必须使用缓冲
fn build_output(items) {
  let buf = Array::new()
  for item in items {
    buf.push(item.to_string())
  }
  buf.join("")
}
```

### 3. 假 I/O（只写内存）
```
// ❌ 不合格：日志只存到内存数组
fn log(msg) { memory_array.push(msg) }

// ✅ 合格：至少支持回调输出
fn log(msg, sink: Sink) { sink.write(msg) }
```

### 4. 假执行（只建模不执行）
```
// ❌ 不合格：构建工具没有执行能力
fn build(graph) { topological_sort(graph) }  // 排序完就结束了

// ✅ 合格：有执行框架
fn build(graph, executor: Executor) {
  for action in topological_sort(graph) {
    executor.execute(action)
  }
}
```

### 5. 假流式（每次从头重解析）
```
// ❌ 不合格
fn stream_parse(accumulated_data) {
  parse(accumulated_data)  // 每次新数据来，从头解析全部
}

// ✅ 合格
fn stream_parse(new_chunk, continuation) {
  continuation.resume(new_chunk)  // 从断点继续
}
```

### 6. 空壳实现
```
// ❌ 绝对禁止
fn shrink(value) { value }  // 不做任何收缩
fn validate(ir) { Ok(()) }  // 不做任何验证
fn optimize(code) { code }  // 不做任何优化

// ✅ 每个函数必须有实质性的实现
```

---

## 绝对禁止的工作模式

1. **禁止"先占位后补全"**：每个提交的函数必须是完整可工作的
2. **禁止"只改简单的跳过难的"**：按 AGENTS.md 要求，改进必须全面，不能选择性修改
3. **禁止"测试通过就行"**：测试通过是最低要求，代码必须是正确、高效、可维护的
4. **禁止"复制粘贴驱动开发"**：共享逻辑必须提取到共享包，禁止跨 spec 复制相同代码
5. **禁止"英文回复"**：全程中文交互，无例外
