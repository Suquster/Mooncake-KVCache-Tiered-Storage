"""pytest 公共夹具与路径接线。

把仓库的 ``python/`` 源码目录加入 ``sys.path``，使测试无需先安装即可
``import bench`` / ``import vllm_adapter``。同时暴露若干路径常量与 cmake 可执行文件
的定位夹具，供冒烟测试复用。
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

# 仓库根目录：本文件位于 <repo>/tests/conftest.py。
REPO_ROOT = Path(__file__).resolve().parents[1]
PYTHON_SRC = REPO_ROOT / "python"

# 让 python/ 下的包可被直接导入。
if str(PYTHON_SRC) not in sys.path:
    sys.path.insert(0, str(PYTHON_SRC))


@pytest.fixture(scope="session")
def repo_root() -> Path:
    """返回仓库根目录路径。"""
    return REPO_ROOT


@pytest.fixture(scope="session")
def cmake_executable() -> str:
    """定位 cmake 可执行文件。

    优先使用 PATH 中的 cmake；找不到时回退到 pip 安装的 ``cmake`` 包自带二进制。
    若两者皆无则跳过依赖 cmake 的测试，避免在缺少构建工具的环境中误报失败。
    """
    found = shutil.which("cmake")
    if found:
        return found
    try:
        import cmake  # type: ignore

        candidate = Path(cmake.__file__).parent / "data" / "bin" / "cmake"
        if candidate.is_file():
            return str(candidate)
    except Exception:  # noqa: BLE001 - 仅用于优雅降级
        pass
    pytest.skip("环境中未找到 cmake 可执行文件，跳过依赖 cmake 的检查")
