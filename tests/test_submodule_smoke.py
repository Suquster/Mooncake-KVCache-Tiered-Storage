"""任务 1.1 冒烟测试：子模块完整性与构建失败信息（需求 6.3 / 6.5）。

覆盖两条断言：

1. 需求 6.3：``mooncake.lock`` 同时记录了合法的 commit 哈希与版本号，且与上游
   子模块 ``third_party/mooncake`` 的实际版本一致（子模块完整性）。
2. 需求 6.5：当锁定的 Mooncake 版本不可用（子模块缺失）时，构建期校验脚本以
   非零状态失败，且失败信息中**明确指明所需的 Mooncake 版本**。

校验逻辑的唯一可信实现位于 ``cmake/CheckMooncakeVersion.cmake``，本测试通过
``cmake -P`` 脚本模式直接驱动它，确保「构建会失败的那段逻辑」本身被测试覆盖。
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from bench.provenance import read_mooncake_lock

# 上游版本号在子模块中的权威位置（相对子模块根目录），与 CMake 校验脚本保持一致。
_UPSTREAM_VERSION_FILE = "mooncake-wheel/pyproject.toml"
_CHECK_SCRIPT_REL = "cmake/CheckMooncakeVersion.cmake"


# ---------------------------------------------------------------------------
# 需求 6.3：mooncake.lock 记录 commit 哈希 + 版本，且与子模块一致
# ---------------------------------------------------------------------------
@pytest.mark.smoke
def test_lock_records_commit_hash_and_version(repo_root: Path) -> None:
    """mooncake.lock 必须同时记录合法的 commit 哈希与版本号。"""
    lock = read_mooncake_lock(repo_root / "mooncake.lock")

    assert lock.has_commit_hash(), (
        f"mooncake.lock 的 commit 字段应为 40 位十六进制完整哈希，实际为：{lock.commit!r}"
    )
    assert lock.has_version(), (
        f"mooncake.lock 的 version 字段应为合法语义化版本，实际为：{lock.version!r}"
    )
    # tag 与仓库地址作为溯源信息也应存在。
    assert lock.tag, "mooncake.lock 应记录上游 tag"
    assert lock.repo and lock.repo.endswith("Mooncake.git"), "mooncake.lock 应记录上游仓库地址"


@pytest.mark.smoke
def test_submodule_present_and_version_matches_lock(repo_root: Path) -> None:
    """子模块存在且其实际版本与 mooncake.lock 锁定版本一致（子模块完整性）。"""
    lock = read_mooncake_lock(repo_root / "mooncake.lock")
    version_file = repo_root / "third_party" / "mooncake" / _UPSTREAM_VERSION_FILE

    if not version_file.is_file():
        pytest.skip(
            "上游 Mooncake 子模块未初始化（third_party/mooncake 为空）；"
            "请运行 git submodule update --init --recursive 后重试"
        )

    text = version_file.read_text(encoding="utf-8")
    assert f'version = "{lock.version}"' in text, (
        f"子模块实际版本与 mooncake.lock 锁定的 {lock.version} 不一致"
    )


# ---------------------------------------------------------------------------
# 需求 6.5：锁定版本不可用时，构建以「指明所需版本」的信息失败
# ---------------------------------------------------------------------------
@pytest.mark.smoke
def test_build_check_fails_with_required_version_message(
    repo_root: Path, cmake_executable: str, tmp_path: Path
) -> None:
    """当子模块缺失时，版本校验脚本必须失败并在信息中指明所需 Mooncake 版本。"""
    lock = read_mooncake_lock(repo_root / "mooncake.lock")
    check_script = repo_root / _CHECK_SCRIPT_REL
    assert check_script.is_file(), f"缺少构建期校验脚本：{check_script}"

    # 指向一个空目录模拟「锁定版本不可用 / 子模块缺失」的场景。
    missing_submodule_dir = tmp_path / "missing_mooncake"
    missing_submodule_dir.mkdir()

    # 注意：脚本模式下 -D 定义必须置于 -P 之前。
    proc = subprocess.run(
        [
            cmake_executable,
            f"-DREPO_ROOT={repo_root}",
            f"-DMOONCAKE_LOCK_FILE={repo_root / 'mooncake.lock'}",
            f"-DMOONCAKE_SUBMODULE_DIR={missing_submodule_dir}",
            "-P",
            str(check_script),
        ],
        capture_output=True,
        text=True,
    )

    combined = proc.stdout + proc.stderr

    # 1) 构建（校验）必须失败。
    assert proc.returncode != 0, (
        "锁定的 Mooncake 版本缺失时，版本校验应以非零状态失败，"
        f"但实际返回 0；输出：\n{combined}"
    )
    # 2) 失败信息必须明确指明所需的 Mooncake 版本。
    assert lock.version in combined, (
        f"构建失败信息中应包含所需的 Mooncake 版本 {lock.version}；实际输出：\n{combined}"
    )


@pytest.mark.smoke
def test_build_check_passes_with_correct_submodule(
    repo_root: Path, cmake_executable: str
) -> None:
    """子模块版本正确时，版本校验脚本应成功通过（回归保护）。"""
    submodule_dir = repo_root / "third_party" / "mooncake"
    if not (submodule_dir / _UPSTREAM_VERSION_FILE).is_file():
        pytest.skip("上游 Mooncake 子模块未初始化，跳过正向校验")

    check_script = repo_root / _CHECK_SCRIPT_REL
    proc = subprocess.run(
        [
            cmake_executable,
            f"-DREPO_ROOT={repo_root}",
            f"-DMOONCAKE_LOCK_FILE={repo_root / 'mooncake.lock'}",
            f"-DMOONCAKE_SUBMODULE_DIR={submodule_dir}",
            "-P",
            str(check_script),
        ],
        capture_output=True,
        text=True,
    )
    combined = proc.stdout + proc.stderr
    assert proc.returncode == 0, f"子模块版本正确时校验不应失败；输出：\n{combined}"
