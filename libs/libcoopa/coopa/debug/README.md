# Debug Module

The `debug` module provides thread-safe console logging interfaces and queue-based diagnostic managers suitable for multithreaded jobs.

## Component Architecture

- **[`Printer`](./printer.h)**: Abstract base logging interface.
- **[`Logger`](./logger.h)**: Thread-safe, direct synchronous console output logger that formats events with level flags, caller tags, and microsecond timestamps.
- **[`LogMessage`](./message.h)**: Simple value structure holding logged strings, thread flags, categories, and high-resolution timestamps.
- **[`DebugContext`](./context.h)**: Subclass of `Printer` that writes logs into a `ParallelQueue` rather than immediately executing console writes.
- **[`DebugManager`](./manager.h)**: Diagnostic scheduler context helper that flushes queue logs, sorts them chronologically, and dumps them.
- **[`DebugBucket`](./bucket.h)**: Similar to the DebugManager, providing thread-local or bucket-level sorting buffers for high-performance isolated logging.

## Usage Patterns

### Direct Console Logger

Best for synchronous initialization scripts:

```cpp
#include <coopa/debug/logger.h>

void sample() {
    coopa::debug::Logger logger("Engine");
    logger.info("Initializing engine resources...");
    logger.warn("Unresolved assets prefix fallback triggered.");
}
```

### Queue-Based Parallel Diagnostics Manager

Best for worker threads where interleaving stdout is undesirable:

```cpp
#include <coopa/debug/manager.h>

void sample_parallel() {
    coopa::debug::DebugManager manager;
    auto& ctx = manager.get_context();

    // Log from multiple worker threads
    ctx.info("Job started", "WORKER_1");
    ctx.info("Asset processed", "WORKER_2");

    // Flush and print chronologically
    manager.show();
}
```
