// Phase 3 — synthetic order flow generator.
//
// Realistic means cancel-heavy. An order book benchmarked on flow that is
// mostly adds is a benchmark of the case that does not happen (README.md,
// "Why this exists").
//
// Parameters and their realistic defaults, from the build manual:
//
//   cancel ratio          0.85–0.95   matches real equity order-to-trade ratios
//   aggressive fraction   0.05–0.15   most orders rest; a minority cross
//   price distribution    concentrated near touch, thinning outward
//                                     — uniform prices give a flat book and
//                                       flattering cache behaviour
//   order size            log-normal  many small, few large; drives multi-level
//                                     sweeps
//   steady-state depth    10–100      depth changes level-scan cost
//
// The seed is a parameter and is recorded in the output, so any run in
// RESULTS.md can be reproduced exactly.

// TODO(phase 3): the generator.
