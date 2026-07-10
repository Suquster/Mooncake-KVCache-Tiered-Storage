---
inclusion: always
---

# 自动执行规则（Auto-Execute Directive）

**绝对禁止暂停等待用户确认。** 所有决策自己按最高标准判断并直接执行。

---

## 强制规则

1. **不要问用户确认** — 任何需要做出选择的地方，直接按最高标准/最佳实践执行
2. **git commit 自动做** — 每完成一个阶段的修改，自动 commit，不要问"是否提交"
3. **git push 自动做** — 每次 commit 后立即执行 `git push origin main`，不要问"是否推送"。git remote 已配置好认证，可以直接 push。
4. **abort/todo!/panic 占位** — 全部改为返回 Result 或 Option，不要问"保留还是改"
5. **边界决策** — 全部按最安全、最健壮的方案处理，不要暂停询问
6. **循环不要停** — 每完成一轮优化，直接进入下一轮，不要说"要继续吗"
7. **不要总结等待** — 不要在每轮结束后暂停等用户说"继续"，直接继续

## Git 自动提交推送流程

每完成一个阶段的修改后，在终端执行：
```bash
cd /home/ubuntu/moonbit-pathfinding
moon info && moon fmt && moon test
git add -A
git commit -m "描述本次修改的 commit message"
git push origin main
```
不要等待确认，直接执行。remote 已配置 PAT 认证，push 不需要额外密码。

## 唯一允许暂停的情况

- 遇到编译错误无法解决（连续 3 次修复失败）
- 遇到测试全面失败（非个别测试）
- 需要用户提供外部信息（密钥、账号等）

除以上 3 种情况外，**任何暂停都是违规**。
