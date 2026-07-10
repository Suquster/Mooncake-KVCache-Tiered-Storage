#!/bin/bash
# Push current HEAD to the GitHub remote for this repo.
# Usage: ./pushmain.sh [branch=main]
# Auth: export GitHubPAT=<token>
set -e
TARGET_BRANCH="${1:-main}"
if [ -z "${GitHubPAT:-}" ]; then
  echo "错误: 未设置 GitHubPAT 环境变量" >&2
  exit 1
fi
git push "https://${GitHubPAT}@github.com/Suquster/Mooncake-KVCache-Optimization-Contest-2026.git" "HEAD:refs/heads/${TARGET_BRANCH}"
echo "Pushed to ${TARGET_BRANCH} successfully"
