"""真实 vLLM 在线基准：前缀复用型多轮对话负载，测 TTFT / 吞吐。

对 OpenAI 兼容端点发起多会话多轮请求（长共享系统前缀 + 会话内历史累积，
复现 KVCache 复用场景），流式读取首 token 时延（TTFT）与端到端时延。
用于对比：GPU-only 基线 / CPU 卸载 lru / CPU 卸载 s3fifo（本项目策略）。

用法：
    python3 bench_online.py --base-url http://localhost:8000 \
        --model Qwen/Qwen2.5-7B-Instruct --sessions 24 --rounds 4 \
        --prefix-tokens 6000 --concurrency 8 --tag baseline
"""

from __future__ import annotations

import argparse
import asyncio
import json
import random
import statistics
import time

import aiohttp

_WORDS = (
    "存储 分层 缓存 淘汰 预取 命中 前缀 复用 推理 显存 内存 块 键值 会话 "
    "调度 传输 引擎 模型 上下文 令牌"
).split()


def _make_text(rng: random.Random, tokens: int) -> str:
    return " ".join(rng.choice(_WORDS) for _ in range(tokens))


def _load_sharegpt(
    path: str, sessions: int, prefix_chars: int, rng: random.Random
) -> tuple[list[str], list[list[str]]]:
    """从 ShareGPT JSON 构建真实会话：返回 (会话前缀列表, 每会话轮次问题列表)。

    前缀 = 长文档式拼接的真实对话文本（裁到 prefix_chars），轮次问题取自
    同一会话的真实 human 发言（裁短），复现真实 token 分布与多轮复用。
    """
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    convs = [
        [t["value"] for t in c.get("conversations", []) if t.get("value")]
        for c in data
    ]
    convs = [c for c in convs if len(c) >= 4 and sum(len(x) for x in c) >= prefix_chars]
    rng.shuffle(convs)
    assert len(convs) >= sessions, f"可用会话不足: {len(convs)} < {sessions}"
    prefixes: list[str] = []
    questions: list[list[str]] = []
    for c in convs[:sessions]:
        prefixes.append(" ".join(c)[:prefix_chars])
        qs = [t[:400] for t in c if t.strip()][:16]
        questions.append(qs)
    return prefixes, questions


async def _one_request(
    session: aiohttp.ClientSession,
    base_url: str,
    model: str,
    prompt: str,
    max_tokens: int,
) -> tuple[float, float, int]:
    """返回 (TTFT 秒, 总时长秒, 生成 token 数)。"""
    payload = {
        "model": model,
        "prompt": prompt,
        "max_tokens": max_tokens,
        "temperature": 0.0,
        "stream": True,
    }
    start = time.perf_counter()
    ttft = None
    tokens = 0
    async with session.post(
        f"{base_url}/v1/completions", json=payload
    ) as resp:
        resp.raise_for_status()
        async for raw in resp.content:
            line = raw.decode().strip()
            if not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if data == "[DONE]":
                break
            chunk = json.loads(data)
            if chunk.get("choices") and chunk["choices"][0].get("text"):
                if ttft is None:
                    ttft = time.perf_counter() - start
                tokens += 1
    total = time.perf_counter() - start
    return (ttft if ttft is not None else total, total, tokens)


async def run(args: argparse.Namespace) -> None:
    rng = random.Random(42)
    # 每会话独立扩展前缀 + 多轮历史累积（round r 复用 round r-1 的前缀）。
    sessions: list[str] = []
    session_questions: list[list[str]] | None = None
    if args.sharegpt:
        # 真实数据集：每会话前缀与轮次问题均来自 ShareGPT 真实对话。
        # 粗估 1 token ≈ 4 字符（英文）。
        sessions, session_questions = _load_sharegpt(
            args.sharegpt, args.sessions, args.prefix_tokens * 4, rng
        )
    else:
        shared_prefix = _make_text(rng, args.prefix_tokens)
        for s in range(args.sessions):
            sessions.append(shared_prefix + " " + _make_text(rng, 512))

    sem = asyncio.Semaphore(args.concurrency)
    results: list[tuple[float, float, int]] = []

    async with aiohttp.ClientSession(
        timeout=aiohttp.ClientTimeout(total=600)
    ) as http:

        async def run_session(idx: int) -> None:
            history = sessions[idx]
            for r in range(args.rounds):
                if session_questions is not None:
                    qs = session_questions[idx]
                    question = qs[r % len(qs)]
                else:
                    question = _make_text(rng, 32)
                prompt = history + " 用户问题：" + question
                async with sem:
                    ttft, total, tokens = await _one_request(
                        http, args.base_url, args.model, prompt,
                        args.max_tokens,
                    )
                results.append((ttft, total, tokens))
                history = prompt + " " + _make_text(rng, args.max_tokens)

        async def run_scanner(idx: int) -> None:
            # 扫描干扰：每轮全新长前缀（零复用），冲刷缓存但不计入统计。
            srng = random.Random(1000 + idx)
            for _ in range(args.rounds):
                prompt = _make_text(srng, args.prefix_tokens) + " 摘要："
                async with sem:
                    await _one_request(
                        http, args.base_url, args.model, prompt,
                        args.max_tokens,
                    )

        start = time.perf_counter()
        await asyncio.gather(
            *(run_session(i) for i in range(args.sessions)),
            *(run_scanner(i) for i in range(args.scan_sessions)),
        )
        wall = time.perf_counter() - start

    ttfts = sorted(t for t, _, _ in results)
    total_tokens = sum(k for _, _, k in results)
    p = lambda q: ttfts[min(len(ttfts) - 1, int(q * len(ttfts)))]
    print(
        json.dumps(
            {
                "tag": args.tag,
                "requests": len(results),
                "ttft_p50_s": round(statistics.median(ttfts), 3),
                "ttft_p95_s": round(p(0.95), 3),
                "ttft_mean_s": round(statistics.mean(ttfts), 3),
                "wall_s": round(wall, 1),
                "gen_tokens": total_tokens,
                "tokens_per_s": round(total_tokens / wall, 1),
            },
            ensure_ascii=False,
        )
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://localhost:8000")
    parser.add_argument("--model", default="Qwen/Qwen2.5-7B-Instruct")
    parser.add_argument("--sessions", type=int, default=24)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--prefix-tokens", type=int, default=6000)
    parser.add_argument("--max-tokens", type=int, default=64)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--scan-sessions", type=int, default=0)
    parser.add_argument(
        "--sharegpt",
        default="",
        help="ShareGPT JSON 路径；非空则用真实对话构建会话前缀与问题",
    )
    parser.add_argument("--tag", default="run")
    args = parser.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
