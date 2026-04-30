# Performance Comparison Capability

## Purpose

This document defines how to produce credible performance comparisons in RecStore.
It is project-specific and focused on the current benchmark layout, transport paths, memory modes, and reporting conventions.

## Scope

Use this capability when comparing:

- different storage backends or memory placements
- different transport paths
- the same path under different capacity or resource limits
- RecStore against an external baseline
- a new optimization against an existing in-repo baseline

The expected output is a comparison that is reproducible, scoped, and explicit about what was actually compared.

## Comparison Rules

### Keep resource assumptions explicit

A path name is not enough. State the effective resource model for each path, for example:

- GPU present with HBM used for vectors
- GPU present but HBM not used for vectors
- shared-memory local path
- RPC path

Do not collapse these into a single “architecture-level” conclusion.

### Separate throughput from bring-up behavior

Initialization failures are not throughput results. Keep them separate. Common pre-steady-state failure surfaces include:

- server startup readiness
- benchmark binary path drift
- initialization timeout
- initialization OOM
- transport-specific startup artifacts

If a path fails during initialization, report it as a reachability or initialization limit.

### Keep curves dense and tables compact

The full sweep and the report table serve different purposes:

- the curve should keep dense points so trend changes are visible
- the table should only keep highlighted scales

### Treat capacity as part of the experiment definition

In RecStore, capacity can change validity, reachability, and fairness.
Whenever RecStore paths are involved, record the capacity policy for the measured points.

### Do not confuse transport conclusions with storage conclusions

A shared-memory result and an RPC result answer different questions:

- a shared-memory result says something about a local fast path
- an RPC result says something about a networked transport path

Do not use an RPC result as proof of storage-layer inferiority or superiority.

## Required Record

Every reusable comparison should record:

- experiment goal
- compared path classes
- resource assumptions for each class
- workload definition
- measured scale points
- repeat policy
- capacity policy for RecStore paths
- aggregated throughput results
- any initialization or reachability limits
- location of raw records

## Repository Pitfalls

- Benchmark helper defaults may point to stale binary locations. Verify the actual built binary paths before trusting a runner.
- Startup readiness detection can be fragile if it depends on exact log text. Treat server-ready parsing as an implementation detail that may need hardening.
- Large-table RPC runs may fail during initialization before any meaningful steady-state number exists. Report this as an initialization limit.
- Shared-memory comparisons depend on explicit capacity sizing per measured scale. Reusing one fixed capacity across scales can invalidate the result.
- This environment may not have `pdflatex`; TeX reports may need text-level verification without local PDF compilation.

## Preferred Output

Preferred report shape:

- one short goal statement
- one compact highlighted table
- one dense trend figure
- one short conclusion
- one short methodology block at the end
