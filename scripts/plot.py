#!/usr/bin/env python3
"""Turn raw ob_bench latency samples into percentile tables and figures.

Every figure in RESULTS.md is regenerated from CSV by this script. Nothing is
hand-made, so a stale plot is impossible and a reviewer can rerun it.

Input contract (written by bench/bench_main.cpp, long format, one row per
measured operation):

    engine,operation,latency_ns
    naive,cancel,412
    fast,cancel,38

Aggregation deliberately happens here and not in C++, so the raw sample set
stays on disk and any statistic can be recomputed without rerunning the
benchmark.

Outputs, into the directory given by --outdir:

    latency_percentiles.csv   the table, machine readable
    latency_percentiles.md    the same table as markdown, to paste into
                              RESULTS.md
    latency_percentiles.png   p50/p99/p99.9 by operation, engines side by side
    latency_tail.png          complementary CDF per operation, log-log

Usage:
    python3 scripts/plot.py data/results.csv
    python3 scripts/plot.py data/results.csv --outdir docs/figures --title "run 7"
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Percentiles reported everywhere in this project. Means are deliberately
# absent: in latency-sensitive work the tail is the product, and a mean hides
# exactly the behaviour being measured.
PERCENTILES = [50.0, 99.0, 99.9, 99.99]

# Engines in a fixed order so every figure and table reads the same way, and so
# the baseline is always the left-hand bar.
ENGINE_ORDER = ["naive", "reference", "fast", "optimised"]

REQUIRED_COLUMNS = {"engine", "operation", "latency_ns"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Percentile tables and figures from ob_bench output.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("csv", type=Path, help="raw sample CSV from ob_bench --out")
    p.add_argument(
        "--outdir",
        type=Path,
        default=None,
        help="where to write tables and figures (default: alongside the input CSV)",
    )
    p.add_argument(
        "--title",
        default="",
        help="suffix for figure titles, e.g. a run label or date",
    )
    p.add_argument(
        "--no-figures",
        action="store_true",
        help="write the tables only; skip matplotlib entirely",
    )
    return p.parse_args(argv)


def load(csv_path: Path):
    try:
        import pandas as pd
    except ImportError:
        sys.exit(
            "pandas is required.\n"
            "    pip install pandas matplotlib"
        )

    if not csv_path.is_file():
        sys.exit(f"no such file: {csv_path}")

    df = pd.read_csv(csv_path)

    missing = REQUIRED_COLUMNS - set(df.columns)
    if missing:
        sys.exit(
            f"{csv_path} is missing column(s): {', '.join(sorted(missing))}\n"
            f"expected header: engine,operation,latency_ns"
        )

    if df.empty:
        sys.exit(f"{csv_path} has a header but no samples")

    df["latency_ns"] = df["latency_ns"].astype("float64")

    negative = int((df["latency_ns"] < 0).sum())
    if negative:
        # Usually an unsynchronised rdtsc read across cores. Worth stopping for
        # rather than quietly reporting a percentile computed from nonsense.
        sys.exit(
            f"{negative} negative latency sample(s) in {csv_path}. "
            "Check the timer: an unpinned thread reading rdtsc across cores "
            "produces these."
        )

    return df


def engine_sort_key(engine: str) -> tuple[int, str]:
    name = str(engine).lower()
    return (ENGINE_ORDER.index(name) if name in ENGINE_ORDER else len(ENGINE_ORDER), name)


def summarise(df):
    """One row per (operation, engine) with the reported percentiles."""
    import numpy as np
    import pandas as pd

    rows = []
    for (operation, engine), group in df.groupby(["operation", "engine"], sort=False):
        samples = group["latency_ns"].to_numpy()
        row = {
            "operation": operation,
            "engine": engine,
            "samples": int(samples.size),
        }
        # `lower` rather than the default linear interpolation: a reported
        # latency should be a value that was actually observed.
        for q in PERCENTILES:
            label = f"p{q:g}".replace(".", "_")
            row[label] = float(np.percentile(samples, q, method="lower"))
        row["max"] = float(samples.max())
        rows.append(row)

    summary = pd.DataFrame(rows)
    summary["_engine_key"] = summary["engine"].map(engine_sort_key)
    summary = (
        summary.sort_values(["operation", "_engine_key"])
        .drop(columns="_engine_key")
        .reset_index(drop=True)
    )
    return summary


def percentile_columns(summary) -> list[str]:
    return [c for c in summary.columns if c.startswith("p")] + ["max"]


def write_markdown(summary, path: Path) -> None:
    cols = percentile_columns(summary)
    headers = ["Operation", "Engine", "Samples"] + [
        c.replace("_", ".") if c != "max" else "max" for c in cols
    ]

    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join(["---"] * len(headers)) + "|",
    ]
    for _, r in summary.iterrows():
        cells = [str(r["operation"]), str(r["engine"]), f"{int(r['samples']):,}"]
        cells += [f"{r[c]:,.0f}" for c in cols]
        lines.append("| " + " | ".join(cells) + " |")

    lines.append("")
    lines.append("All figures nanoseconds per operation. Generated by "
                 "`scripts/plot.py`; do not edit by hand.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def plot_percentiles(summary, path: Path, title_suffix: str) -> None:
    import numpy as np
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    shown = ["p50", "p99", "p99_9"]
    shown = [c for c in shown if c in summary.columns]

    operations = list(dict.fromkeys(summary["operation"]))
    engines = sorted(set(summary["engine"]), key=engine_sort_key)

    fig, axes = plt.subplots(
        1, len(shown), figsize=(4.2 * len(shown), 4.4), sharey=True
    )
    if len(shown) == 1:
        axes = [axes]

    x = np.arange(len(operations))
    width = 0.8 / max(len(engines), 1)

    for ax, col in zip(axes, shown):
        for i, engine in enumerate(engines):
            values = []
            for op in operations:
                match = summary[
                    (summary["operation"] == op) & (summary["engine"] == engine)
                ]
                values.append(float(match[col].iloc[0]) if not match.empty else np.nan)
            ax.bar(x + i * width - 0.4 + width / 2, values, width, label=str(engine))

        ax.set_title(col.replace("_", "."))
        ax.set_xticks(x)
        ax.set_xticklabels(operations, rotation=20, ha="right")
        # Log scale because the engines are expected to differ by an order of
        # magnitude; on a linear axis the faster one is an invisible stub.
        ax.set_yscale("log")
        ax.grid(axis="y", alpha=0.3, which="both")

    axes[0].set_ylabel("latency (ns, log scale)")
    axes[-1].legend()

    title = "Latency percentiles by operation"
    if title_suffix:
        title += f" — {title_suffix}"
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def plot_tail(df, path: Path, title_suffix: str) -> None:
    """Complementary CDF: for each latency, the fraction of operations slower.

    This is the plot that shows tail shape rather than three points from it,
    and it is where a difference in outlier behaviour becomes visible.
    """
    import numpy as np
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    operations = list(dict.fromkeys(df["operation"]))
    engines = sorted(set(df["engine"]), key=engine_sort_key)

    fig, axes = plt.subplots(
        1, len(operations), figsize=(4.2 * len(operations), 4.2), sharey=True
    )
    if len(operations) == 1:
        axes = [axes]

    for ax, op in zip(axes, operations):
        for engine in engines:
            samples = df[(df["operation"] == op) & (df["engine"] == engine)][
                "latency_ns"
            ].to_numpy()
            if samples.size == 0:
                continue
            samples = np.sort(samples)
            # Fraction of samples strictly slower than each value.
            ccdf = 1.0 - np.arange(samples.size) / samples.size
            ax.plot(samples, ccdf, label=str(engine), linewidth=1.4)

        ax.set_title(str(op))
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("latency (ns)")
        ax.grid(alpha=0.3, which="both")

    axes[0].set_ylabel("P(latency > x)")
    axes[-1].legend()

    title = "Latency tail (complementary CDF)"
    if title_suffix:
        title += f" — {title_suffix}"
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    df = load(args.csv)
    outdir = args.outdir or args.csv.parent
    outdir.mkdir(parents=True, exist_ok=True)

    summary = summarise(df)

    csv_out = outdir / "latency_percentiles.csv"
    md_out = outdir / "latency_percentiles.md"
    summary.to_csv(csv_out, index=False)
    write_markdown(summary, md_out)
    print(f"wrote {csv_out}")
    print(f"wrote {md_out}")

    if not args.no_figures:
        try:
            import matplotlib  # noqa: F401
        except ImportError:
            print(
                "matplotlib not installed — skipping figures "
                "(pip install matplotlib)",
                file=sys.stderr,
            )
        else:
            bars = outdir / "latency_percentiles.png"
            tail = outdir / "latency_tail.png"
            plot_percentiles(summary, bars, args.title)
            plot_tail(df, tail, args.title)
            print(f"wrote {bars}")
            print(f"wrote {tail}")

    print()
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
