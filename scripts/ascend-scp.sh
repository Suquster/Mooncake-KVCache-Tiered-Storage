#!/bin/bash
# 昇腾机 scp 工具（凭据从环境变量读取）: ./scripts/ascend-scp.sh <local_path> <remote_path>
: "${ASCEND_HOST:?请设置 ASCEND_HOST 环境变量}"
: "${ASCEND_PORT:=22}"
: "${ASCEND_USER:?请设置 ASCEND_USER 环境变量}"
: "${ASCEND_PASS:?请设置 ASCEND_PASS 环境变量}"
SSHPASS="$ASCEND_PASS" exec sshpass -e scp -o StrictHostKeyChecking=no -P "$ASCEND_PORT" -r "$1" "${ASCEND_USER}@${ASCEND_HOST}:$2"
