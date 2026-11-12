# Asynchronous generation and performance

## Injecting a job engine

Generation and export can run off the calling thread, on a `coopa::job::JobEngine` (from
libcoopa) that the caller provides. mapcoopa never owns an engine, so a host that already
has a thread pool shares it. With no engine set, the default, everything runs inline.

```cpp
coopa::job::JobEngine engine(8);

coopa::maps::MapGenerator generator(config, logger);
generator.set_job_engine(&engine);              // nullptr (the default) runs inline
coopa::maps::MapTask task = generator.generate_async();

while (!task.done()) {                          // poll from a frame loop
    draw_loading_bar(task.progress());          // 0..1
}
use(generator.graph());
```

Exporting has the same shape:

```cpp
coopa::maps::MapExporter exporter;
exporter.set_job_engine(&engine);
coopa::maps::MapTask task = exporter.export_layers_async(graph, config, "world");
```

`MapTask` ([`map_task.h`](../coopa/maps/map_task.h)) has `done()`, `progress()`,
`cancel()` and `wait()`. It is move-only, and **its destructor cancels and waits**. The work
writes into storage the caller owns (the generator's graph, the exporter's images), so a
task must not outlive it. `export_layers_async()` keeps its arguments by reference, so keep
the graph and config alive until the task finishes.

`cancel()` is cooperative. Generation checks between passes; export checks before each
layer and between row bands. A PNG encode already in progress cannot be interrupted.

## Where the time goes

Generation itself is small (about 100 to 150 ms for the default map). Rendering and
encoding the nine 4800 x 4800 layers is almost all of a run. A recent default run
(`--seed=42`, 8 worker threads on an Apple Silicon Mac, Release build):

| | generate | export (9 layers) | yaml |
|---|---|---|---|
| `--threads=1` (serial) | 93 ms | 14.6 s | 369 ms |
| `--threads=0` (8 workers) | 107 ms | 6.9 s | 363 ms |

The tool prints these timings at the end of every run.

Two things matter most:

- **Build type.** The standalone CMake build defaults to `Release`. An unoptimised build
  is several times slower (an older measurement: 40.3 s of export at `-O0` serial against
  9.5 s optimised serial).
- **Threads.** Export is split across layers and across row bands within a layer.
  `--threads=1` runs serially; `--threads=0` (the default) uses every core.
  `--png-level` trades file size for encode time.

Generation is not parallelised. It is too short for threading to help, and the road pass
cannot be: each route is routed over ground earlier routes already claimed.

## Parallel output is identical to serial output

Work is split only by disjoint output: across layers, and across row bands within a layer.
Splitting by cell would race, because adjacent cells share boundary pixels. So a parallel
run is byte-for-byte identical to a serial one. To check:

```bash
./build/mapcoopa --seed=42 --threads=1 --out=/tmp/serial
./build/mapcoopa --seed=42 --threads=0 --out=/tmp/parallel
cmp /tmp/serial.yaml /tmp/parallel.yaml
for l in elevation water biomes roads structures landmarks regions composite caves; do
    cmp /tmp/serial_$l.png /tmp/parallel_$l.png || echo "DIFFERS: $l"
done
```
