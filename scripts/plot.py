#!/usr/bin/env python3
"""Turn raw ob_bench latency samples into percentile tables, figures, and the
dashboard's data payload.

Every figure in RESULTS.md and every number in docs/dashboard.html is
regenerated from CSV by this script. Nothing is hand-made, so a stale plot or
a stale dashboard is impossible and a reviewer can rerun it.

Input contract (written by bench/bench_main.cpp, long format, one row per
measured operation):

    engine,operation,latency_ns,seed
    naive,cancel,412,1
    fast,cancel,38,1

`seed` is optional: when absent, each input file counts as one run. Pass every
run's CSV at once to get run-to-run spread —

    python3 scripts/plot.py data/fast_s*.csv data/naive_s*.csv --outdir docs/figures

Aggregation deliberately happens here and not in C++, so the raw sample set
stays on disk and any statistic can be recomputed without rerunning the
benchmark.

Outputs, into the directory given by --outdir:

    latency_percentiles.csv   the table, machine readable
    latency_percentiles.md    the same table as markdown, to paste into
                              RESULTS.md
    latency_percentiles.png   p50/p99/p99.9 by operation, engines side by side
    latency_tail.png          complementary CDF per operation, log-log
    dashboard_data.json       percentiles + spread + tail curves for
                              docs/dashboard.html (see scripts/build_dashboard.py)

Usage:
    python3 scripts/plot.py data/results.csv
    python3 scripts/plot.py data/*.csv --outdir docs/figures --title "run 7"
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Percentiles reported everywhere in this project. Means are deliberately
# absent: in latency-sensitive work the tail is the product, and a mean hides
# exactly the behaviour being measured.
PERCENTILES = [50.0, 99.0, 99.9, 99.99]

# The dashboard carries one extra step (p90) so its table can show the shape
# between the median and the knee.
DASHBOARD_PERCENTILES = {
    "p50": 50.0,
    "p90": 90.0,
    "p99": 99.0,
    "p999": 99.9,
    "p9999": 99.99,
}

# Engines in a fixed order so every figure and table reads the same way, and so
# the baseline is always the left-hand bar.
ENGINE_ORDER = ["naive", "reference", "fast", "optimised"]

# Operation order for the dashboard: the cancel path first, because it is the
# one the engine is designed around.
OPERATION_ORDER = ["cancel", "add_passive", "add_aggressive", "reduce"]

REQUIRED_COLUMNS = {"engine", "operation", "latency_ns"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Percentile tables and figures from ob_bench output.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "csv",
        type=Path,
        nargs="+",
        help="raw sample CSV(s) from ob_bench --out; pass every run to get spread",
    )
    p.add_argument(
        "--outdir",
        type=Path,
        default=None,
        help="where to write tables and figures (default: alongside the first CSV)",
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
    p.add_argument(
        "--no-dashboard-data",
        action="store_true",
        help="skip dashboard_data.json",
    )
    return p.parse_args(argv)


def load(csv_paths: list[Path]):
    try:
        import pandas as pd
    except ImportError:
        sys.exit(
            "pandas is required.\n"
            "    pip install pandas matplotlib"
        )

    frames = []
    for csv_path in csv_paths:
        if not csv_path.is_file():
            sys.exit(f"no such file: {csv_path}")

        df = pd.read_csv(csv_path)

        missing = REQUIRED_COLUMNS - set(df.columns)
        if missing:
            sys.exit(
                f"{csv_path} is missing column(s): {', '.join(sorted(missing))}\n"
                f"expected header: engine,operation,latency_ns[,seed]"
            )

        if df.empty:
            sys.exit(f"{csv_path} has a header but no samples")

        df["latency_ns"] = df["latency_ns"].astype("float64")

        # One "run" per seed when the column is there, else per input file —
        # so spread is never inferred from a filename convention.
        if "seed" in df.columns:
            df["run"] = df["seed"].astype(str)
        else:
            df["run"] = csv_path.stem

        negative = int((df["latency_ns"] < 0).sum())
        if negative:
            # Usually an unsynchronised rdtsc read across cores. Worth stopping
            # for rather than quietly reporting a percentile computed from
            # nonsense.
            sys.exit(
                f"{negative} negative latency sample(s) in {csv_path}. "
                "Check the timer: an unpinned thread reading rdtsc across cores "
                "produces these."
            )

        frames.append(df)

    return pd.concat(frames, ignore_index=True) if len(frames) > 1 else frames[0]


def engine_sort_key(engine: str) -> tuple[int, str]:
    name = str(engine).lower()
    return (ENGINE_ORDER.index(name) if name in ENGINE_ORDER else len(ENGINE_ORDER), name)


def operation_sort_key(operation: str) -> tuple[int, str]:
    name = str(operation)
    return (
        OPERATION_ORDER.index(name) if name in OPERATION_ORDER else len(OPERATION_ORDER),
        name,
    )


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


def write_markdown(summary, path: Path, note: str = "") -> None:
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
    lines.append(note or "All figures nanoseconds per operation. Generated by "
                         "`scripts/plot.py`; do not edit by hand.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def median_run_frame(df, payload):
    """Restrict to each engine's median run.

    Every artifact this script writes — table, figures, dashboard — then
    describes the *same* run, so RESULTS.md and docs/dashboard.html cannot
    disagree. Pooling runs instead would report a percentile of a mixture,
    which is not a latency any single run exhibited.
    """
    keep = None
    for engine, run in payload["median_run"].items():
        mask = (df["engine"].astype(str) == engine) & (df["run"].astype(str) == run)
        keep = mask if keep is None else (keep | mask)
    return df[keep] if keep is not None else df


def _percentiles_of(samples) -> dict:
    import numpy as np

    out = {
        label: float(np.percentile(samples, q, method="lower"))
        for label, q in DASHBOARD_PERCENTILES.items()
    }
    out["max"] = float(samples.max())
    out["n"] = int(samples.size)
    return out


def _ccdf_points(samples, per_decade: int = 14) -> list[list[float]]:
    """Rank-sampled survival curve: log-spaced ranks, x = the latency observed
    at that rank. Faithful to the tail's shape without shipping every sample
    to the browser."""
    import numpy as np

    ordered = np.sort(samples)[::-1]
    n = ordered.size
    ranks = np.unique(
        np.clip(
            np.round(
                np.logspace(0, np.log10(n), num=int(np.log10(n) * per_decade) + 1)
            ),
            1,
            n,
        ).astype(int)
    )
    return [[int(ordered[k - 1]), float(k / n)] for k in ranks]


def dashboard_payload(df) -> dict:
    """Percentiles, run-to-run spread and tail curves, keyed engine→operation.

    The reported run is the *median* run by p50 on the reference operation
    (cancel where present), so no single lucky run can be presented as the
    result — the same median-of-runs rule RESULTS.md states.
    """
    engines = sorted(set(df["engine"].astype(str)), key=engine_sort_key)
    operations = sorted(set(df["operation"].astype(str)), key=operation_sort_key)
    reference_op = "cancel" if "cancel" in operations else operations[0]

    payload = {
        "ops": operations,
        "engines": engines,
        "reference_op": reference_op,
        "median_run": {},
        "runs": {},
        "percentiles": {},
        "ccdf": {},
    }

    for engine in engines:
        eng_df = df[df["engine"].astype(str) == engine]
        runs = sorted(set(eng_df["run"].astype(str)))
        payload["runs"][engine] = runs

        # Median run by p50 on the reference operation.
        import numpy as np

        scored = []
        for run in runs:
            s = eng_df[
                (eng_df["run"].astype(str) == run)
                & (eng_df["operation"].astype(str) == reference_op)
            ]["latency_ns"].to_numpy()
            if s.size:
                scored.append((float(np.percentile(s, 50, method="lower")), run))
        scored.sort()
        median_run = scored[len(scored) // 2][1] if scored else runs[0]
        payload["median_run"][engine] = median_run

        payload["percentiles"][engine] = {}
        payload["ccdf"][engine] = {}
        for op in operations:
            op_df = eng_df[eng_df["operation"].astype(str) == op]
            if op_df.empty:
                continue

            per_run = {}
            for run in runs:
                s = op_df[op_df["run"].astype(str) == run]["latency_ns"].to_numpy()
                if s.size:
                    per_run[run] = _percentiles_of(s)

            reported = per_run.get(median_run) or _percentiles_of(
                op_df["latency_ns"].to_numpy()
            )
            spread = {
                key: [
                    min(v[key] for v in per_run.values()),
                    max(v[key] for v in per_run.values()),
                ]
                for key in ("p50", "p99", "p999")
            } if per_run else {}

            payload["percentiles"][engine][op] = {
                "median": reported,
                "range": spread,
                "runs": len(per_run),
            }

            tail_samples = (
                op_df[op_df["run"].astype(str) == median_run]["latency_ns"].to_numpy()
                if median_run in per_run
                else op_df["latency_ns"].to_numpy()
            )
            payload["ccdf"][engine][op] = _ccdf_points(tail_samples)

    return payload


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
    outdir = args.outdir or args.csv[0].parent
    outdir.mkdir(parents=True, exist_ok=True)

    payload = dashboard_payload(df)
    payload["sources"] = sorted(p.name for p in args.csv)

    # Table and figures describe the median run, matching the dashboard.
    reported = median_run_frame(df, payload)
    runs_per_engine = {e: len(r) for e, r in payload["runs"].items()}
    multi_run = any(n > 1 for n in runs_per_engine.values())
    if multi_run:
        picks = ", ".join(f"{e} = {r}" for e, r in payload["median_run"].items())
        print(f"median run per engine ({payload['reference_op']} p50): {picks}")
    note = (
        "All figures nanoseconds per operation, from each engine's median run by "
        f"`{payload['reference_op']}` p50 "
        f"({', '.join(f'{e}: run {r}' for e, r in payload['median_run'].items())}) "
        f"of {runs_per_engine}. Generated by `scripts/plot.py`; do not edit by hand."
        if multi_run else ""
    )

    summary = summarise(reported)

    csv_out = outdir / "latency_percentiles.csv"
    md_out = outdir / "latency_percentiles.md"
    summary.to_csv(csv_out, index=False)
    write_markdown(summary, md_out, note)
    print(f"wrote {csv_out}")
    print(f"wrote {md_out}")

    if not args.no_dashboard_data:
        data_out = outdir / "dashboard_data.json"
        data_out.write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")
        print(f"wrote {data_out} (runs per engine: {runs_per_engine})")

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
            plot_tail(reported, tail, args.title)
            print(f"wrote {bars}")
            print(f"wrote {tail}")

    print()
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
