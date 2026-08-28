#!/usr/bin/env python3
"""Build docs/dashboard.html from the template plus real recorded data.

The dashboard is generated, never hand-edited — the same rule the figures
follow, so it cannot drift from RESULTS.md after an optimisation pass. The
page is self-contained by design (it has to open from a file:// path and
publish as a standalone artifact, neither of which can fetch a sibling JSON),
so the data is injected at build time rather than loaded at runtime.

Inputs:
    docs/dashboard.template.html   the page, with __BENCH__ / __REPLAY__ /
                                   __STAMP__ placeholders
    dashboard_data.json            from scripts/plot.py (percentiles, spread,
                                   tail curves)
    replay.json                    from ob_bench --replay-out (ops + events)

Full pipeline, from a clean build:

    for s in 1 2 3 4 5; do
      ./build/bench/ob_bench --engine=fast  --orders=2000000 --seed=$s --out=data/fast_s$s.csv
      ./build/bench/ob_bench --engine=naive --orders=300000  --seed=$s --out=data/naive_s$s.csv
    done
    ./build/bench/ob_bench --replay-out=data/replay.json --replay-ops=2600 --seed=42
    python3 scripts/plot.py data/*.csv --outdir docs/figures
    python3 scripts/build_dashboard.py --data docs/figures/dashboard_data.json \\
                                       --replay data/replay.json

Usage:
    python3 scripts/build_dashboard.py [--data F] [--replay F] [--out F]
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# What the page's JavaScript indexes into. Checked here so a truncated or
# partial data file fails the build loudly instead of producing a page with
# blank charts.
REQUIRED_OPS = ["cancel", "add_passive", "add_aggressive", "reduce"]
REQUIRED_ENGINES = ["fast", "naive"]


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Inject recorded data into the dashboard template.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--template", type=Path, default=REPO / "docs" / "dashboard.template.html")
    p.add_argument("--data", type=Path, default=REPO / "docs" / "figures" / "dashboard_data.json")
    p.add_argument("--replay", type=Path, default=REPO / "data" / "replay.json")
    p.add_argument("--out", type=Path, default=REPO / "docs" / "dashboard.html")
    return p.parse_args(argv)


def read_json(path: Path, what: str) -> dict:
    if not path.is_file():
        sys.exit(
            f"missing {what}: {path}\n"
            "Run the pipeline in this script's docstring first — the dashboard "
            "is built from recorded data, not committed with it."
        )
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        sys.exit(f"{path} is not valid JSON: {e}")


def validate(bench: dict, replay: dict) -> None:
    for engine in REQUIRED_ENGINES:
        if engine not in bench.get("percentiles", {}):
            sys.exit(f"dashboard data has no '{engine}' engine — run both engines")
        for op in REQUIRED_OPS:
            if op not in bench["percentiles"][engine]:
                sys.exit(f"dashboard data has no '{op}' samples for '{engine}'")

    single = [
        f"{e}"
        for e in REQUIRED_ENGINES
        if bench["percentiles"][e][REQUIRED_OPS[0]].get("runs", 0) < 2
    ]
    if single:
        print(
            f"note: only one run for {', '.join(single)} — the page will show no "
            "seed-to-seed spread. Pass every run's CSV to plot.py for that.",
            file=sys.stderr,
        )

    if not replay.get("ops"):
        sys.exit("replay recording has no operations")


def git_stamp() -> str:
    try:
        rev = subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )
        dirty = subprocess.run(
            ["git", "-C", str(REPO), "status", "--porcelain"],
            capture_output=True, text=True, timeout=10,
        )
        if rev.returncode == 0:
            suffix = "+dirty" if dirty.stdout.strip() else ""
            return rev.stdout.strip() + suffix
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown revision"


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    if not args.template.is_file():
        sys.exit(f"missing template: {args.template}")
    template = args.template.read_text(encoding="utf-8")

    bench = read_json(args.data, "dashboard data (scripts/plot.py)")
    replay = read_json(args.replay, "replay recording (ob_bench --replay-out)")
    validate(bench, replay)

    built = dt.datetime.now().astimezone().strftime("%d %b %Y")
    sources = ", ".join(bench.get("sources", [])) or args.data.name
    runs = bench["percentiles"]["fast"]["cancel"].get("runs", 1)
    stamp = (
        f"Generated {built} from {sources} and {args.replay.name} "
        f"({runs} run(s) per engine, reported run = median by cancel p50) "
        f"at {git_stamp()} by scripts/build_dashboard.py — not hand-edited."
    )

    # Exactly one occurrence each: a placeholder that also appears in a
    # comment would get the whole payload injected into it too, silently
    # doubling the page.
    for placeholder in ("/*__BENCH__*/", "/*__REPLAY__*/", "__STAMP__"):
        found = template.count(placeholder)
        if found != 1:
            sys.exit(
                f"template must contain {placeholder} exactly once, found "
                f"{found}: {args.template}"
            )

    out = (
        template
        .replace("/*__BENCH__*/", json.dumps(bench, separators=(",", ":")))
        .replace("/*__REPLAY__*/", json.dumps(replay, separators=(",", ":")))
        .replace("__STAMP__", stamp)
    )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(out, encoding="utf-8")
    print(f"wrote {args.out} ({args.out.stat().st_size:,} bytes)")
    print(f"  {stamp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
