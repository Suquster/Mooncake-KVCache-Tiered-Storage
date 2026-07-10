"""Hypothesis 属性测试 harness 接线验证 —— mooncake.lock 解析器。

本测试既验证 Python 属性测试 harness（Hypothesis）已正确接线，也为脚手架阶段
新增的公开函数 :func:`bench.provenance.parse_lock_text` 提供属性级覆盖。
每个属性最少运行 100 个样例（项目硬性标准）。

标签格式遵循：Feature: mooncake-kvcache-optimization, Property {n}: {text}
"""

from __future__ import annotations

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from bench.provenance import parse_lock_text

# 每个属性至少 100 次迭代（项目硬性标准）。
_MIN_ITERATIONS = 100

# 合法 KEY 的字符集（与解析器正则 [A-Za-z0-9_]+ 严格一致：仅 ASCII 字母、数字、下划线）。
_KEY_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"
_keys = st.text(alphabet=_KEY_ALPHABET, min_size=1, max_size=24)

# 值不含换行、不以 '#' 起始、首尾无空白（避免与注释/空行/strip 语义冲突）。
_values = (
    st.text(
        alphabet=st.characters(blacklist_characters="\n\r#", blacklist_categories=("Cs", "Cc")),
        min_size=1,
        max_size=64,
    )
    .map(lambda s: s.strip())
    .filter(lambda s: bool(s) and not s.startswith("#"))
)


@pytest.mark.property
@settings(max_examples=_MIN_ITERATIONS)
@given(st.dictionaries(_keys, _values, min_size=1, max_size=8))
def test_parse_lock_text_roundtrip(mapping: dict[str, str]) -> None:
    """Feature: mooncake-kvcache-optimization, Property: KEY=VALUE 解析往返一致。

    对任意合法的键值映射，渲染为 ``KEY=VALUE`` 文本再解析，应还原出相同映射。
    """
    text = "\n".join(f"{key}={value}" for key, value in mapping.items())
    parsed = parse_lock_text(text)
    assert parsed == mapping


@pytest.mark.property
@settings(max_examples=_MIN_ITERATIONS)
@given(
    mapping=st.dictionaries(_keys, _values, min_size=0, max_size=6),
    comments=st.lists(st.text(alphabet=st.characters(blacklist_characters="\n\r"), max_size=40), max_size=5),
)
def test_parse_lock_text_ignores_comments_and_blank_lines(
    mapping: dict[str, str], comments: list[str]
) -> None:
    """Feature: mooncake-kvcache-optimization, Property: 注释与空行被忽略。

    在键值行之间穿插 ``#`` 注释行与空行，解析结果应仅包含键值对。
    """
    lines: list[str] = []
    for key, value in mapping.items():
        lines.append(f"#{comments[0] if comments else ' 注释'}")
        lines.append("")
        lines.append(f"{key}={value}")
    text = "\n".join(lines)
    assert parse_lock_text(text) == mapping
