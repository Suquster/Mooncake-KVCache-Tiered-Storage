"""策略选型（policy shootout）：在真实 FAST'25 trace 上对比全部缓存策略。

用法：``python -m bench.shootout <trace.jsonl> [pool_blocks]``
对每个策略输出命中率与 TTFT p50/p99，按命中率降序排列。
"""

from __future__ import annotations

import sys

from bench.framework import _simulate_replay, build_replay_plan, parse_trace, rates
from bench.policies import POLICIES


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if not args:
        print("用法：python -m bench.shootout <trace.jsonl> [pool_blocks]", file=sys.stderr)
        return 2
    pool = int(args[1]) if len(args) > 1 else 4096
    plan = build_replay_plan(parse_trace(args[0]))

    results = []
    for name in sorted(POLICIES):
        log, ttft, _, throughput = _simulate_replay(plan, pool, name)
        hit_rate, reuse = rates(log)
        ordered = sorted(ttft)
        p50 = ordered[len(ordered) // 2]
        p99 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.99))]
        results.append((hit_rate, name, p50, p99, reuse, throughput))

    results.sort(reverse=True)
    print(f"pool={pool} blocks, trace={args[0]}")
    for hit_rate, name, p50, p99, reuse, throughput in results:
        print(
            f"  {name:>14}: hit={hit_rate:.4f} reuse={reuse:.4f} "
            f"ttft_p50={p50:.1f}ms ttft_p99={p99:.1f}ms tput={throughput:.0f} tok/s"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
