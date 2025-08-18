malloc-bench
------------
This is a benchmark for checking contention on malloc in a multi-threaded context.

Usage
=====
```./bench [-t THREADS] [-c CONCURRENT_ALLOCS] [-n NUM_CONCURRENT_ALLOCS]```

Parameters:
* `-t`: number of threads to run (default: `sysconf`'s `_SC_NPROCESSORS_ONLN`)
* `-c`: number of allocations per batch (default: 64)
* `-n`: number of batches to run (default: 16384)

Methodology
===========
This benchmark measures how well an allocator performs under contention using a variety of allocation sizes. Sizes of 32, 1024, 32,768, and 1,048,576 bytes are used, simulating many common allocation sizes (small to large).

A number of threads `t` are spawned. A loop is run in each thread, where a random bin is picked and a number of allocations `c` are performed. The time this takes is measured. Then the allocations are freed. The time it takes to free all allocations is also measured. This continues until `c x n` allocations are performed.

The time it takes for all threads to finish is also measured.

When all measurements complete, they are aggregated, and the stats are printed, per-bin and for all allocations.

Requirements
============
This should work under any reasonably modern POSIX system, except macOS (which lacks pthread barriers).

Building
========
**Note:** Some systems do not require `-lm` or may choke on it. On those systems, it's usually safe to remove, as long as the C library has math routines included.

`cc bench.c -o bench -O2 -pthread -lm`
