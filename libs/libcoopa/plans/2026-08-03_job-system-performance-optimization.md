# Job System Performance Optimization for Realtime Applications

> **Status: done.** Handles come from a fixed `CounterPool` (in `handle.h`, not a separate `pool.h`), workers use Chase-Lev deques, dependencies resolve event-driven via `dependency_graph.h`, `JobScheduler` clears its persistent hazard handles in `begin_frame()`, and per-type counters are a fixed `std::array` behind `COOPA_JOB_DIAGNOSTICS`. File links below point at an old checkout path.

A comprehensive analysis and optimization plan for the `coopa/job/` module to make it production-ready for game engines and realtime applications.

---

## Analysis Summary

The current job system has a solid conceptual architecture — work-stealing, dependency graphs with hazard detection, thread dedication — but the implementation has **critical performance bottlenecks, structural anti-patterns, and a confirmed memory leak** that would make it unsuitable for a shipped game engine frame loop.

---

## 🐛 Memory Leaks Detected

> [!CAUTION]
> **Confirmed memory leak in [handle.h](file:///home/coopa/git/libcoopa/coopa/job/handle.h)**
> 
> Every `JobHandle` default-constructs a `std::make_shared<std::atomic<int>>(0)`. Handles are **copied freely** throughout the system (into `Job::dependencies` vectors, into tracking maps in the scheduler, returned as vectors from `submit_all()`). The `shared_ptr` ensures the atomic counter itself isn't leaked, **but**:
> 
> 1. **`persistent_last_writer_handles_` and `persistent_active_reader_handles_`** in [scheduler.h:L264-265](file:///home/coopa/git/libcoopa/coopa/job/scheduler.h#L264-L265) are **never cleared between frames**. They accumulate `shared_ptr` references to completed-but-still-alive atomic counters indefinitely. Over thousands of frames, this is a **slow, unbounded memory leak**.
> 2. **`job_type_counts_`** in [engine.h:L517](file:///home/coopa/git/libcoopa/coopa/job/engine.h#L517) is a `std::map<JobType, std::atomic<int>>` that only ever grows — entries are never removed even if a job type is no longer used.

> [!WARNING]
> **Potential leak via `Logger*` raw pointer in [handle.h:L66](file:///home/coopa/git/libcoopa/coopa/job/handle.h#L66)**
> 
> `JobHandle` has `trav::debug::Logger* logger_ = nullptr;` that is never assigned, never used, and takes up 8 bytes per handle for no reason. Not a functional leak, but dead weight in a hot object.

---

## User Review Required

> [!IMPORTANT]
> **Structural Overhaul Scope**: Several of these changes (especially Phase 1 and Phase 2) alter the public API of `JobHandle`, `Job`, and `JobEngine::submit()`. Any code outside this module that uses these types will need updating. Please confirm whether there are consumers beyond [test.cpp](file:///home/coopa/git/libcoopa/coopa/test.cpp) that I should be aware of.

> [!IMPORTANT]
> **Lock-free work-stealing deque**: Phase 2 proposes replacing `std::deque` + `std::mutex` per-thread queues with a Chase-Lev lock-free work-stealing deque. This is the single highest-impact change for realtime performance, but is also the most complex. Do you want me to implement this from scratch, or would you prefer a well-tested open-source implementation (e.g., adapted from `concurrentqueue` or a minimal Chase-Lev)?

---

## Open Questions

1. **Target platform(s)**: Are you targeting x86-64 only, or also ARM (e.g., for mobile/console)? This affects cache-line size assumptions (`64B` vs `128B` on Apple Silicon).
2. **Job allocation budget**: Are you willing to use a fixed-capacity pre-allocated job pool (e.g., 4096 jobs max in-flight), or must the system handle arbitrary dynamic growth?
3. **Frame-based lifecycle**: Should I add explicit `begin_frame()` / `end_frame()` methods to the scheduler for deterministic per-frame cleanup, or keep the current open-ended model?

---

## Proposed Changes

### Phase 1: Eliminate Allocations in the Hot Path

The **single most important** change for realtime. The current design allocates heap memory on every job submission.

---

#### [MODIFY] [handle.h](file:///home/coopa/git/libcoopa/coopa/job/handle.h)

**Problem**: Every `JobHandle` calls `std::make_shared<std::atomic<int>>()` — a **heap allocation per handle**. Handles are copied into vectors, maps, and jobs. Each copy increments/decrements `shared_ptr` ref-counts (atomic operations, cache-line bouncing).

**Change**:
- Replace `shared_ptr<atomic<int>>` with a simple integer index into a pre-allocated counter pool, or a raw `atomic<int>*` pointing into engine-owned storage
- Remove the dead `Logger* logger_` member
- Make `JobHandle` trivially copyable (no destructor side effects), enabling `memcpy`-level performance

---

#### [MODIFY] [job.h](file:///home/coopa/git/libcoopa/coopa/job/job.h)

**Problem**: `std::function<void()>` has a **small-buffer optimization (SBO)** threshold (typically 16-32 bytes). Any capture list exceeding this triggers a heap allocation. `std::vector<JobHandle> dependencies` allocates on every job with dependencies.

**Change**:
- Replace `std::function<void()>` with a fixed-size callable wrapper or a raw function pointer + `void* userdata` pair
- Replace `std::vector<JobHandle> dependencies` with a small fixed-capacity inline array (e.g., `std::array<JobHandle, 4>` + `uint8_t dep_count`) — most jobs have 0-3 dependencies
- Ensure `sizeof(Job)` fits within a single cache line (64 bytes) where possible

---

#### [NEW] [pool.h](file:///home/coopa/git/libcoopa/coopa/job/pool.h)

**Purpose**: Pre-allocated, fixed-capacity job pool to eliminate all per-submission `new`/`malloc` calls.

- Ring-buffer or free-list of `Job` slots
- Atomic index for lock-free allocation from the pool
- `reset()` method for per-frame recycling
- Configurable capacity (default: 4096)

---

### Phase 2: Lock-Free Work-Stealing Deque

The second highest-impact change. The current per-thread queues use `std::deque<Job>` guarded by per-queue `std::mutex` — every push, pop, and steal is a lock acquisition.

---

#### [MODIFY] [engine.h](file:///home/coopa/git/libcoopa/coopa/job/engine.h)

**Problem areas and changes**:

| Current | Problem | Proposed |
|---|---|---|
| `std::deque<Job>` + `std::mutex` per queue | Lock contention on every push/pop/steal | Chase-Lev lock-free work-stealing deque |
| `std::deque<Job> pending_jobs_` + `std::mutex` | Single global lock for all dependency promotions — becomes the bottleneck | Partition pending jobs per-thread, or use a lock-free MPSC queue |
| `std::map<JobType, unsigned int> thread_dedications_` | `std::map` (tree-based, cache-unfriendly) locked on every `submit()` | `std::array` or flat lookup indexed by `JobType` (if types are dense integers) |
| `std::map<JobType, std::atomic<int>> job_type_counts_` | Tree-based map with atomic values; cache-unfriendly, never pruned | `std::array<std::atomic<int>, MAX_JOB_TYPES>` pre-allocated flat array |
| `worker_cv_.notify_one()` on every submit | Condition variable syscalls are expensive (~1-5µs each) | Batch notifications; use exponential backoff spinning before CV wait |
| `try_promote_pending_job()` linear scan | O(N) scan of ALL pending jobs under a global lock | Move to an event-driven approach: when a job completes, directly promote its dependents |
| Global `worker_mutex_` for CV | All idle workers contend on a single mutex | Per-thread or sharded sleep/wake mechanism |

**Detailed structural changes**:

1. **Replace per-thread `std::deque` + `mutex` with Chase-Lev deque**
   - Owner thread pushes/pops from the bottom (no synchronization needed)
   - Thieves steal from the top (single CAS operation)
   - Eliminates all mutex acquisitions in the hot path

2. **Event-driven dependency resolution** (replaces `try_promote_pending_job()`)
   - When `execute_job()` decrements a handle counter to 0, check a reverse map of "jobs waiting on this handle"
   - Directly push newly-ready jobs into worker queues
   - Eliminates the O(N) linear scan

3. **Spinning-then-sleeping worker wait strategy**
   - Workers spin for a configurable number of iterations (e.g., 64-256) before falling back to CV wait
   - Avoids the ~1-5µs syscall overhead for short idle gaps between job batches
   - Critical for maintaining sub-millisecond frame scheduling

4. **Cache-line padding** for all per-thread atomics to prevent false sharing

---

### Phase 3: Scheduler Optimizations

---

#### [MODIFY] [scheduler.h](file:///home/coopa/git/libcoopa/coopa/job/scheduler.h)

| Current | Problem | Proposed |
|---|---|---|
| `persistent_last_writer_handles_` never cleared | Unbounded memory growth (leak) | Clear at `submit_all()` boundaries, or add `begin_frame()`/`end_frame()` lifecycle |
| `persistent_active_reader_handles_` accumulates vectors | Vectors within the map grow unbounded | Clear or recycle per batch |
| Dependency dedup via sort + unique on `shared_ptr` raw pointers | O(N log N) per job | Use a small bitset or flat set for handle IDs (with the new integer-ID handles) |
| Reader-is-also-writer check is O(R×W) nested loop at [L212-218](file:///home/coopa/git/libcoopa/coopa/job/scheduler.h#L212-L218) | Quadratic for jobs with many components | Pre-sort or use a hash set of write types |
| `std::mutex` guards `add_job()` and `submit_all()` | Prevents concurrent `add_job()` during `submit_all()` (correct) but also prevents concurrent `add_job()` calls from different threads | Use a lock-free MPSC queue for `add_job()`, only lock during `submit_all()` |

---

### Phase 4: Thread & Object Cleanup

---

#### [MODIFY] [thread.h](file:///home/coopa/git/libcoopa/coopa/job/thread.h)

- Remove the per-thread `std::mutex` and `std::condition_variable` — they're passed to the worker loop but the engine uses a **single global** `worker_mutex_`/`worker_cv_` instead, making these dead allocations
- Store a `uint32_t` thread ID instead of `unsigned int` for explicit sizing
- Consider making Thread non-copyable, non-movable explicitly (`= delete`)

#### [MODIFY] [collections/queue.h](file:///home/coopa/git/libcoopa/coopa/job/collections/queue.h)

- The `ParallelQueue` exists but is **not used anywhere** in the job system. Either integrate it or note it for removal to avoid confusion.

---

### Phase 5: Diagnostic & Profiling Improvements

---

#### [MODIFY] [engine.h](file:///home/coopa/git/libcoopa/coopa/job/engine.h) (diagnostics section)

- `get_thread_diagnostics()` locks **every per-queue mutex sequentially** — in a 16-thread system this is 16 lock acquisitions. With lock-free queues, this becomes a simple atomic read
- Wrap diagnostics behind a compile-time `#ifdef COOPA_JOB_DIAGNOSTICS` to eliminate overhead in release builds
- Add per-frame timing: jobs executed count, steal count, promotion count, max queue depth

---

## Verification Plan

### Automated Tests

```bash
# Build and run existing tests
cd /home/coopa/git/libcoopa/build && cmake .. && make -j$(nproc) && ./coopa_test
```

- Extend [test.cpp](file:///home/coopa/git/libcoopa/coopa/test.cpp) with:
  - **Throughput benchmark**: Submit 100,000 no-op jobs, measure total time (target: < 10ms on 8 cores)
  - **Dependency chain stress test**: Chain of 1000 sequential jobs, verify ordering
  - **Memory stability test**: Run 10,000 "frames" (submit + wait cycles), verify RSS doesn't grow
  - **Contention test**: N threads simultaneously calling `add_job()` + `submit_all()`

### Manual Verification

- Profile with `perf stat` / `perf record` to verify mutex contention is reduced
- Use `valgrind --tool=massif` to confirm no unbounded memory growth
- Compare before/after frame times when integrated with a simple game loop simulation

---

## Priority Order

| Priority | Phase | Impact | Risk |
|---|---|---|---|
| 🔴 P0 | Fix memory leaks (scheduler persistent maps, dead logger ptr) | Correctness | Low |
| 🔴 P0 | Phase 1: Eliminate hot-path allocations | ~5-10x throughput | Medium |
| 🟠 P1 | Phase 2: Lock-free deques + event-driven deps | ~2-5x throughput under contention | High |
| 🟡 P2 | Phase 3: Scheduler optimizations | ~1.5-2x for complex graphs | Medium |
| 🟢 P3 | Phase 4: Thread/object cleanup | Code quality, minor perf | Low |
| 🟢 P3 | Phase 5: Diagnostics gating | Release build perf | Low |
