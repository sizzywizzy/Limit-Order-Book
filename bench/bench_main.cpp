// Phase 3 — benchmark harness.
//
// Not implemented yet. This target exists so that the build, the CLI contract
// and the output format are settled before any number is produced, because a
// harness designed after the fact tends to be designed around the answer you
// were hoping for.
//
// Output contract (consumed by scripts/plot.py — keep the two in step):
//
//     engine,operation,latency_ns
//     naive,cancel,412
//     fast,cancel,38
//
// One row per measured operation, long format, no aggregation in C++.
// Percentiles are computed in the plot script so that the raw sample set stays
// available and a reviewer can recompute anything.
//
// Measurement rules that must hold in the implementation (RESULTS.md,
// "Measurement rules"): discard warm-up, record per operation type separately,
// fix and print the seed, pin the thread, and report the median of five or
// more runs with the spread.

#include <cstdio>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::fputs(
        "ob_bench: not implemented (phase 3).\n"
        "\n"
        "Planned interface:\n"
        "  ob_bench --engine={naive|fast} --orders=N --cancel-ratio=R\n"
        "           --aggressive-fraction=F --depth=L --seed=S --out=FILE\n"
        "\n"
        "Until then there are no latency figures, and RESULTS.md says so.\n",
        stderr);

    return 1;
}
