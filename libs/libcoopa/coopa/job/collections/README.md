# Concurrent Collections Module

The `job/collections` module provides the concurrent containers the job system runs on, plus two general-purpose thread-safe wrappers offered to consumers.

`ParallelQueue` and `WorkStealingDeque` are load-bearing: `JobEngine` dispatches
every job through them. `ParallelMap` and `ParallelVector` are not used anywhere
inside libcoopa -- they are provided for consumers, and are exercised only by
this repository's own test suite.

## Collection API Details

### 1. `ParallelMap` ([`map.h`](./map.h))
- Wraps `phmap::parallel_flat_hash_map` into a simplified lookup API.
- Individual mutations (`add`, `remove`, `contains`) are internally locked.
- **Iteration warning**: Basic iterators `begin()` and `end()` are safe but do not lock the whole container. For consistent point-in-time traversals, use `snapshot()` or `keys()` to obtain copied structures.

### 2. `ParallelQueue` ([`queue.h`](./queue.h))
- Simple FIFO queue utilizing a mutex lock and conditional variables.
- Methods: `push` (appends items and wakes waiting threads), `try_pop` (returns empty state status if empty, otherwise pops front), `pop_all`, and `clear`.

### 3. `ParallelVector` ([`vector.h`](./vector.h))
- Mutual-exclusion locked wrapper on `std::vector`.
- Methods: `push_back`, `try_pop_back`, `try_pop_front`, `push_back_all` (atomic merge), `pop_all` (flush-to-vector), `at` (bounds checked access), and `remove_one`/`remove_all` item matching filters.

## Usage Example

```cpp
#include <coopa/job/collections/queue.h>

void producer_consumer_sample() {
    coopa::job::ParallelQueue<int> tasks;
    
    // Thread A: Push tasks
    tasks.push(42);
    
    // Thread B: Safely try pop tasks
    int item;
    if (tasks.try_pop(item)) {
        // Process item (42)
    }
}
```
