"""mooncake.lock 依赖溯源解析。

本模块是 Python 侧读取「锁定的上游 Mooncake 版本/commit」的唯一入口，被两处复用：

* 基准框架在每次运行的报告中回写 commit 哈希，保证结果可溯源（需求 5.5）。
* 子模块完整性冒烟测试断言锁定文件同时记录了 commit 哈希与版本（需求 6.3 / 任务 1.1）。

锁定文件为纯文本 ``KEY=VALUE`` 格式，``#`` 开头为注释，与 CMake/Shell 解析保持一致。
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

# 完整 Git commit 哈希为 40 位十六进制；用作 commit 字段的格式校验常量。
_FULL_COMMIT_HASH_RE = re.compile(r"^[0-9a-f]{40}$")
# 语义化版本（含上游使用的 ``.postN`` 后缀），用作 version 字段的格式校验常量。
_VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(\.post\d+)?$")
# 单行 KEY=VALUE 解析（忽略首尾空白）。
_KV_LINE_RE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$")

# 锁定文件中必须存在的关键字段。
_REQUIRED_KEYS = ("mooncake_commit", "mooncake_version")


@dataclass(frozen=True)
class MooncakeLock:
    """mooncake.lock 的结构化表示。"""

    commit: str
    version: str
    tag: str | None
    repo: str | None
    license: str | None
    raw: dict[str, str]

    def has_commit_hash(self) -> bool:
        """commit 字段是否为合法的 40 位十六进制完整哈希。"""
        return bool(_FULL_COMMIT_HASH_RE.match(self.commit))

    def has_version(self) -> bool:
        """version 字段是否为合法的语义化版本号。"""
        return bool(_VERSION_RE.match(self.version))


def _default_lock_path() -> Path:
    """推断仓库根目录下的 mooncake.lock 路径。

    本文件位于 ``<repo>/python/bench/provenance.py``，向上三级即仓库根目录。
    """
    return Path(__file__).resolve().parents[2] / "mooncake.lock"


def parse_lock_text(text: str) -> dict[str, str]:
    """把锁定文件文本解析为 ``KEY=VALUE`` 字典（忽略注释与空行）。"""
    result: dict[str, str] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        match = _KV_LINE_RE.match(line)
        if match:
            result[match.group(1)] = match.group(2)
    return result


def read_mooncake_lock(lock_path: str | Path | None = None) -> MooncakeLock:
    """读取并解析 mooncake.lock。

    参数:
        lock_path: 锁定文件路径；默认为仓库根目录下的 ``mooncake.lock``。

    返回:
        解析后的 :class:`MooncakeLock`。

    异常:
        FileNotFoundError: 锁定文件不存在。
        ValueError: 缺少关键字段（commit / version）。
    """
    path = Path(lock_path) if lock_path is not None else _default_lock_path()
    if not path.is_file():
        raise FileNotFoundError(f"未找到 mooncake.lock：{path}")

    fields = parse_lock_text(path.read_text(encoding="utf-8"))

    missing = [key for key in _REQUIRED_KEYS if not fields.get(key)]
    if missing:
        raise ValueError(
            f"mooncake.lock 缺少必需字段：{', '.join(missing)}（路径：{path}）"
        )

    return MooncakeLock(
        commit=fields["mooncake_commit"],
        version=fields["mooncake_version"],
        tag=fields.get("mooncake_tag"),
        repo=fields.get("mooncake_repo"),
        license=fields.get("mooncake_license"),
        raw=fields,
    )
