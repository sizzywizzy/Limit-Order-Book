#!/usr/bin/env python3
"""Latency versus book depth — the scaling claim, measured.

The reference engine finds an order by scanning the book, so its cancel cost
grows with the number of resting orders. The optimised engine hashes an id to
a slot and unlinks four pointers, so its cost does not. That is the project's
central design argument, and this script is what turns it from an assertion
about complexity classes into a plot.

Depth is the generator's target resting levels per side; the flow keeps the
book populated at roughly four times that, so sweeping depth sweeps the book
size the engines actually work against.

Usage:
    python3 scripts/sweep_depth.py --bench ./build/bench/ob_bench
    python3 scripts/sweep_depth.py --bench ./build/bench/ob_bench \\
        --depths 10,50,100,500,1000 --outdir docs/figures

Writes into --outdir:
    latency_vs_depth.csv   engine, depth, resting, p50, p99, p99.9 per row
    latency_vs_depth.md    the same, as a markdown table for RESULTS.md
    latency_vs_depth.png   cancel p50/p99 against depth, log-log
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

DEFAULT_DEPTHS = [10, 25, 50, 100, 250, 500, 1000, 2000]

# The reference engine's cancel is O(resting), so a run at depth 2000 costs
# ~200x one at depth 10. Fewer operations there keeps the sweep to a couple of
# minutes without changing what is measured — the percentiles are per
# operation, not per run.
OPS_FAST = 400_000
OPS_NAIVE = 60_000

PERCENTILES = [50.0, 99.0, 99.9]


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Sweep book depth and plot cancel latency against it.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--bench", type=Path, required=True, help="path to ob_bench")
    p.add_argument("--outdir", type=Path, default=REPO / "docs" / "figures")
    p.add_argument("--depths", default=",".join(str(d) for d in DEFAULT_DEPTHS))
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--ops-fast", type=int, default=OPS_FAST)
    p.add_argument("--ops-naive", type=int, default=OPS_NAIVE)
    p.add_argument("--operation", default="cancel",
                   help="which operation class to plot")
    return p.parse_args(argv)


def run_one(bench: Path, engine: str, depth: int, ops: int, seed: int,
            out_csv: Path) -> int:
    """Run one configuration; returns the steady-state resting order count."""
    cmd = [
        str(bench), f"--engine={engine}", f"--orders={ops}", f"--depth={depth}",
        f"--seed={seed}", f"--out={out_csv}",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"ob_bench failed for {engine} depth={depth}:\n{proc.stderr}")
    m = re.search(r"resting=(\d+)", proc.stdout)
    return int(m.group(1)) if m else 0


def percentiles_of(csv_path: Path, operation: str) -> dict:
    import numpy as np
    import pandas as pd

    df = pd.read_csv(csv_path)
    v = df[df["operation"] == operation]["latency_ns"].to_numpy()
    if v.size == 0:
        sys.exit(f"no '{operation}' samples in {csv_path}")
    out = {f"p{q:g}".replace(".", "_"): float(np.percentile(v, q, method="lower"))
           for q in PERCENTILES}
    out["samples"] = int(v.size)
    return out


def plot(rows: list[dict], path: Path, operation: str) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    styles = {
        "fast": {"color": "#2a72d8", "marker": "o"},
        "naive": {"color": "#c07c10", "marker": "s"},
    }
    for engine in ("naive", "fast"):
        pts = [r for r in rows if r["engine"] == engine]
        pts.sort(key=lambda r: r["depth"])
        x = [r["resting"] for r in pts]
        st = styles[engine]
        ax.plot(x, [r["p50"] for r in pts], label=f"{engine} · p50",
                linewidth=2, **st)
        ax.plot(x, [r["p99"] for r in pts], label=f"{engine} · p99",
                linewidth=1.4, linestyle="--", **st)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("resting orders in the book (steady state)")
    ax.set_ylabel(f"{operation} latency (ns)")
    ax.set_title(f"{operation} latency vs book size — O(N) scan against O(1) unlink")
    ax.grid(alpha=0.3, which="both")
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def write_markdown(rows: list[dict], path: Path, operation: str) -> None:
    lines = [
        f"| Depth | Resting orders | Engine | {operation} p50 | p99 | p99.9 |",
        "|---|---|---|---|---|---|",
    ]
    for r in sorted(rows, key=lambda r: (r["depth"], r["engine"] != "naive")):
        lines.append(
            f"| {r['depth']:,} | {r['resting']:,} | {r['engine']} | "
            f"{r['p50']:,.0f} | {r['p99']:,.0f} | {r['p99_9']:,.0f} |"
        )
    lines.append("")
    lines.append("Nanoseconds per operation. Generated by `scripts/sweep_depth.py`; "
                 "do not edit by hand.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not args.bench.is_file():
        sys.exit(f"no such binary: {args.bench}")
    depths = [int(d) for d in args.depths.split(",")]
    args.outdir.mkdir(parents=True, exist_ok=True)

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for depth in depths:
            for engine, ops in (("naive", args.ops_naive), ("fast", args.ops_fast)):
                out_csv = Path(tmp) / f"{engine}_d{depth}.csv"
                resting = run_one(args.bench, engine, depth, ops, args.seed, out_csv)
                pct = percentiles_of(out_csv, args.operation)
                row = {"engine": engine, "depth": depth, "resting": resting,
                       "p50": pct["p50"], "p99": pct["p99"], "p99_9": pct["p99_9"],
                       "samples": pct["samples"]}
                rows.append(row)
                print(f"{engine:6s} depth={depth:5d} resting={resting:6d} "
                      f"p50={row['p50']:8.0f} p99={row['p99']:9.0f} "
                      f"({row['samples']:,} samples)")

    csv_out = args.outdir / "latency_vs_depth.csv"
    with csv_out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {csv_out}")

    md_out = args.outdir / "latency_vs_depth.md"
    write_markdown(rows, md_out, args.operation)
    print(f"wrote {md_out}")

    try:
        import matplotlib  # noqa: F401
    except ImportError:
        print("matplotlib not installed — skipping the figure", file=sys.stderr)
    else:
        png = args.outdir / "latency_vs_depth.png"
        plot(rows, png, args.operation)
        print(f"wrote {png}")

    # The headline of the sweep: how much each engine degrades end to end.
    for engine in ("naive", "fast"):
        pts = sorted([r for r in rows if r["engine"] == engine],
                     key=lambda r: r["depth"])
        if len(pts) >= 2:
            growth = pts[-1]["p50"] / pts[0]["p50"]
            print(f"{engine}: p50 grew {growth:.2f}x from {pts[0]['resting']:,} "
                  f"to {pts[-1]['resting']:,} resting orders")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
