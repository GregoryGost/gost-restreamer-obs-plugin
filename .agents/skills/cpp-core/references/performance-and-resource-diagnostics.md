# C++ performance, resources and regressions

A benchmark, profiler sample or RSS chart is evidence with limits, not proof of a defect.

## Baseline

- Establish representative data, exact compiler/toolchain, optimization level, feature flags, architecture, threading, warmup, build type and host load.
- Compare repeated measurements, noise and distribution. A single quick microbenchmark cannot prove a production latency or throughput SLO.
- Validate the functional contract after any optimization. Avoid copying complexity into code for a hypothetical future workload.
- Distinguish CPU, IO, lock contention, allocator churn, cache locality and lifetime retention before changing data structures.

## Memory and resource accounting

- Heap allocations, stack, memory mappings, GPU buffers, native library allocations and OS RSS have different ownership/measurement surfaces.
- ASan/LSan can detect some bad accesses/leaks but are subject to platform/instrumentation limits; a clean run is not a whole-program proof.
- Persistent object caches, delayed Qt deletion, pools, retained callbacks or allocator arenas can increase memory without proving an unintended leak.
- Inspect file handles, sockets, threads, timers and host/plugin lifetime, especially partial startup and unload.
- Use targeted heap, CPU or lock profilers only when already available and the selected diagnostic scope permits collection and retention. Profile output can expose secrets.

## Regression tests

Compare relevant repeatable benchmark matrices. If the optimization concerns shutdown, verify repeated create/start/stop/destroy under failure and contention. Any PGO, link-time optimization or compiler ABI change is opt-in; do not silently activate it merely to pass a performance objective.
