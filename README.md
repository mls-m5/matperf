# matperf

Scope-based C++ profiling with Chrome trace JSON output. The current public header
requires C++20 (`std::source_location`), so consumers must enable C++20 as well as
the library target.

## Basic Usage

```cpp
#include <matperf/profiler.h>

void prepareFrame() {
    PROFILE_FUNCTION();
    // Work to measure. Avoid a scope for every tiny hot-loop iteration.
}

int main() {
    ProfilerSettings settings;
    settings.outputPath = "trace.json";
    enableProfiling(settings); // Initialize once before measured work.
    setProfilerThreadName("main");
    prepareFrame();
} // Normal shutdown joins the writer and finishes the JSON document.
```

Link the CMake target `matperf` and explicitly request `cxx_std_20` on the consumer.
Read the resulting JSON in a compatible Chrome/Perfetto trace viewer. Abnormal
process termination can leave an unfinished trace. Current serialization is JSON,
not the proposed future binary format.

## Scope Labels and Measurement Boundaries

- `PROFILE_FUNCTION()` uses source-location strings with static lifetime.
- `PROFILE_PART("frame preparation")` names a scope. Labels are borrowed
  `std::string_view`s: use string literals or storage that survives profiling and
  background flushing, not temporary strings.
- The macros declare the same local variable name. Use separate lexical scopes
  for multiple profiling scopes within one function.
- `profileInstant(value)` copies its argument, but instant-event collection/export
  is incomplete in the current implementation; do not rely on it as a completion
  marker until that path is implemented and tested.
- Trace coarse work batches (parser feeds, frame preparation, rendering), not every
  byte/cell of a hot loop. Instrumentation can dominate or prevent optimization of
  very small operations. Compare against an uninstrumented baseline.

## Buffering and Overload

Each producer thread currently has a 10,000-slot ring (one slot is reserved to
separate full/empty). Each duration uses entry and exit records. A background
thread periodically serializes records. Sustained trace volume can therefore
become disk/serialization limited.

`shouldWaitOnOverflow` defaults to false: overflow throws. Waiting instead changes
application latency and throughput. In particular, throwing while recording a
scope destructor can terminate the process; overflow is not a safe drop policy.
Do not use tracing at unbounded event rates. Record output location and overflow
settings alongside benchmark results.

## Implementation Caveats / Follow-up Work

These are findings from source inspection, not claims of validated thread safety:

- Ring read/write indices and the background writer's running flag are plain
  fields accessed across threads. Review the memory model and implement proper
  synchronization before trusting concurrent tracing or sanitizer-clean behavior.
- `readSpan()` advances the consumer position before the writer has serialized the
  returned spans. A producer may reuse those slots while serialization reads them.
  Reclamation needs to follow consumption, not merely span acquisition.
- Review thread registration, per-thread naming/instant updates, and final flush
  ordering together with the ring protocol; adding atomics to indices alone is
  not a complete lifetime fix.
- Trace strings are currently emitted without general JSON escaping. Restrict
  names to simple literals until escaping is implemented and tested.
- Check output-file open/write errors explicitly. A trace file's existence alone
  does not prove successful capture or valid JSON.
- Consider an explicit stop/flush lifecycle and a nonthrowing overflow/drop policy.
- A binary format could reduce serialization/storage overhead later; it does not
  by itself solve synchronization or event-rate overhead.

Applications using `fork`/PTY process creation should start profiling's background
thread only after creating the child, or arrange a separately reviewed safe
process-start strategy. Do not assume profiling across fork is supported.
