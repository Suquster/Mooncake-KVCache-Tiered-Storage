#!/bin/bash
# 昇腾验证机 SSH 连接脚本（凭据一律从环境变量读取，严禁写入仓库）
# 用法:
#   export ASCEND_HOST=... ASCEND_PORT=... ASCEND_USER=... ASCEND_PASS=...
#   ./scripts/ascend-ssh.sh                # 交互式登录
#   ./scripts/ascend-ssh.sh '<command>'    # 远程执行命令
: "${ASCEND_HOST:?请设置 ASCEND_HOST 环境变量}"
: "${ASCEND_PORT:=22}"
: "${ASCEND_USER:?请设置 ASCEND_USER 环境变量}"
: "${ASCEND_PASS:?请设置 ASCEND_PASS 环境变量}"

if ! command -v sshpass >/dev/null 2>&1; then
  echo "需要 sshpass: sudo apt-get install -y sshpass" >&2
  exit 1
fi
SSHPASS="$ASCEND_PASS" exec sshpass -e ssh -o StrictHostKeyChecking=no -p "$ASCEND_PORT" "${ASCEND_USER}@${ASCEND_HOST}" "$@"
