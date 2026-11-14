# lowlat_lab

Low-latency C++ components, built and measured one at a time: correctness first,
one change per step, and only numbers that were measured (on one Apple Silicon
machine, with the caveats stated next to each result).

| Project | Status | What it is |
|---|---|---|
| [spsc](spsc/) | done | Single-producer single-consumer queues: mutex baseline, naive ring, padded acquire/release ring, ring with cached counters. Tests, sanitizers, throughput and latency benchmarks, hardware-counter profiles. |
| [lob](lob/) | in progress | Limit order book matching engine. Single-threaded core (price-time priority, cancels, depth views, trade callback) is written; unit tests, a reference model with differential fuzzing, and a benchmark harness are next. |
| pipeline | planned | Ingress thread, queue, engine thread, output, joining the projects above. |

Start with [spsc/README.md](spsc/README.md) for results, build instructions and
the design notes.