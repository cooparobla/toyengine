# Implementation Plan: Add Comments, Docstrings, and README Files to Coopa Modules

> **Status: done.** Every header has Doxygen comments and most module folders have a README (`coopa/event/` and `coopa/yaml/` are documented in their headers and the top-level README only).

## Introduction
This plan outlines the approach to adding proper Doxygen-style comments and docstrings to all headers in the `coopa` library and creating detailed README documentation for each module directory. All documentation will adhere to the rules in `AGENTS.md` (e.g. standard Doxygen tags for C++ declarations, Google Style docstrings for Python if any, etc.).

## Implementation Phases

### Phase 1: Submodule Documentation & Doxygen Comments
In this phase, we will walk through all C++ headers inside the `coopa/` directory and add comprehensive Doxygen comments.

1. **Collections Module**:
   - `coopa/collections/yaml_map.h`: Document the `YAMLMap` class, constructors, template methods for standard and vector I/O, raw accessors, and utility functions.
2. **Debug Module**:
   - `coopa/debug/printer.h`: Document the abstract `Printer` base class.
   - `coopa/debug/context.h`: Document `DebugContext` class and its overridden logging functions.
   - `coopa/debug/message.h`: Document the simple `LogMessage` value-type struct.
   - `coopa/debug/manager.h`: Document `DebugManager` queue-sorting show loops.
   - `coopa/debug/logger.h`: Document the thread-safe standard console `Logger` implementation.
   - `coopa/debug/bucket.h`: Document `DebugBucket` thread-local/bucketed logs.
3. **Util Module**:
   - `coopa/util/math.h`: Document column-major `Mat4` matrix representations and `MathUtil` static helpers.
   - `coopa/util/file.h`: Document environment-aware `FileUtil` asset/resource paths resolver.
   - `coopa/util/string.h`: Document header-only `StringUtil` helper functions.
   - `coopa/util/id.h`: Document thread-safe static `IdUtil` generator.
4. **Job Module**:
   - `coopa/job/job.h`: Document struct `Job` representing unit of work.
   - `coopa/job/handle.h`: Document class `JobHandle` tracker.
   - `coopa/job/thread.h`: Document `Thread` worker wrapper.
   - `coopa/job/engine.h`: Document `JobEngine` work-stealing job processor.
   - `coopa/job/collections/map.h`, `queue.h`, `vector.h`: Document thread-safe concurrent collection adapters (`ParallelMap`, `ParallelQueue`, `ParallelVector`).
   - `coopa/job/scheduler.h`: Document hazard-dependency graph and main thread loop job submissions.

### Phase 2: Create Module READMEs
Create structured and comprehensive markdown READMEs for every module:
1. `coopa/README.md`: Overview of the entire codebase submodules.
2. `coopa/collections/README.md`: Explaining YAML configurations parsing.
3. `coopa/debug/README.md`: Explaining diagnostic/logging systems.
4. `coopa/util/README.md`: Math, Files, Strings, IDs helper functions.
5. `coopa/job/README.md`: The threadpool Job Engine and automatic dependency Job Scheduler.
6. `coopa/job/collections/README.md`: Thread-safe containers map/queue/vector.

---

## Testing and Validation

### Automated Validation
We will compile the repository to verify that adding comments has introduced no syntax or build compilation issues:
```bash
cmake -S . -B build && cmake --build build
```
