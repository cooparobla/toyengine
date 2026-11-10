# Implementation Plan - Generate libcoopa Module Tests

> **Status: done.** The suite lives in `test.cpp` at the repository root (not `coopa/test.cpp`) and now covers every module; see the top-level README's Testing section.

**Date**: 2026-08-03
**Goal**: Generate a robust test suite in `coopa/test.cpp` to verify all libcoopa modules.

## Introduction
The `libcoopa` library consists of four major folders (`util`, `collections`, `job`, `debug`) implementing basic thread-safe utility structures, config deserializers, loggers, and concurrent job work-stealing schedulers. To guarantee regression safety, we need to construct unit tests for each module inside `coopa/test.cpp`.

## Implementation Phases

### Phase 1: Test Framework Scaffold
- Write a light assertion runner (`ASSERT_EQ`, `ASSERT_TRUE`, `RUN_TEST`) in `coopa/test.cpp` using basic helper methods to print status.

### Phase 2: Implement Utility Tests
- Write test functions for `StringUtil`, `IdUtil`, `MathUtil`, `Mat4`, and `FileUtil`.

### Phase 3: Implement Collections & Debug Tests
- Test YAML parsing and serialization using `YAMLMap`.
- Test `Logger`, `DebugBucket`, and `DebugManager`.

### Phase 4: Implement Concurrent Job Collections & Engine Tests
- Write concurrent unit tests for `ParallelQueue`, `ParallelVector`, and `ParallelMap`.
- Test `JobEngine` and thread execution/dedication.
- Test `JobScheduler` hazard management (RAW, WAW, WAR dependencies).

## Testing and Validation
Build and execute the test executable with:
```bash
cbuild
cplay
```
Ensure all tests print success.
