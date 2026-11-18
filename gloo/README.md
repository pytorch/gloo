# Gloo

Collective communications library with various primitives for multi-machine training.

This folder holds the Gloo sources: collective algorithms (`allreduce`, `allgather`, `broadcast`, `barrier`, …), transport implementations (`tcp`, `ibverbs`, `uv`), rendezvous key-value stores, and tests/benchmarks. It backs the `"gloo"` backend used for CPU-side distributed training.
