<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./media/gloo_100k_dark.svg">
    <img width="55%" src="./media/gloo_100k_light.svg" alt="Gloo">
  </picture>
</p>

<h3 align="center">
Collective communications library with various primitives for multi-machine training.
</h3>

<p align="center">
  | <a href="https://github.com/facebookincubator/gloo/tree/main/docs"><b>Gloo Documentation</b></a>
  | <a href="https://pytorch.org/docs/stable/distributed.html"><b>PyTorch Distributed Documentation</b></a>
  | <a href="https://docs.google.com/presentation/d/1BX4o0ggV0-1MLwlLYcFkZHgNThyZX1-IJM3qbm0cmaA/edit?usp=sharing"><b>Introduction to Gloo Presentation</b></a>
  |
</p>
<p align="center">
  <a href="https://opensource.fb.com/support-ukraine"><img alt="Support Ukraine" src="https://img.shields.io/badge/Support-Ukraine-FFD500?style=flat&labelColor=005BBB"></a>
  <a href="https://github.com/facebookincubator/gloo/actions/workflows/build-linux.yml"><img src="https://github.com/facebookincubator/gloo/actions/workflows/build-linux.yml/badge.svg" alt="CI-Linux"></a>

</p>

---


Gloo is a collective communications library. It comes with a number of
collective algorithms useful for machine learning applications. These
include a barrier, broadcast, and allreduce.

Transport of data between participating machines is abstracted so that
IP can be used at all times, or InifiniBand (or RoCE) when available.
In the latter case, if the InfiniBand transport is used, [GPUDirect][gpudirect]
can be used to accelerate cross machine GPU-to-GPU memory transfers.

[gpudirect]: https://developer.nvidia.com/gpudirect

Where applicable, algorithms have an implementation that works with
system memory buffers, and one that works with NVIDIA GPU memory
buffers. In the latter case, it is not necessary to copy memory between
host and device; this is taken care of by the algorithm implementations.

## Requirements

Gloo is built to run on Linux and has no hard dependencies other than libstdc++.
That said, it will generally only be useful when used in combination with a few
optional dependencies below.

Optional dependencies are:
* [CUDA][cuda] and [NCCL][nccl] -- for CUDA aware algorithms, tests, and benchmark
* [Google Test][gtest] -- to build and run tests
* [Hiredis][hiredis] -- for coordinating machine rendezvous through Redis
* [MPI][mpi] -- for coordinating machine rendezvous through MPI

[cuda]: http://www.nvidia.com/object/cuda_home_new.html
[nccl]: https://github.com/nvidia/nccl
[gtest]: https://github.com/google/googletest
[hiredis]: https://github.com/redis/hiredis
[mpi]: https://www.open-mpi.org/

## Documentation

Please refer to [docs/](docs/) for detailed documentation.

## Building

You can build Gloo using CMake.

Since it is a library, it is most convenient to vendor it in your own
project and include the project root in your own CMake configuration.

### Test

Building the tests requires Google Test version 1.8 or higher. On
Ubuntu, this version ships with version 17.10 and up. If you run an
older version, you'll have to install Google Test yourself, and set
the `GTEST_ROOT` CMake variable.

You can install Google Test using conda with:
``` shell
conda install -c anaconda gmock gtest
```
Be carefull that you might need to fish for a package that works with your glibc


To build the tests, run:

``` shell
mkdir -p build
cd build
cmake ../ -DBUILD_TEST=1 -DGTEST_ROOT=/some/path (if using custom install)
make
ls -l gloo/test/gloo_test*
```

To test the CUDA algorithms, specify `USE_CUDA=ON` as well, and the
CUDA tests are built at `gloo/test/gloo_test_cuda`.

### Benchmark


First install the dependencies required by the benchmark tool. On
Ubuntu, you can do so by running:

``` shell
sudo apt-get install -y libhiredis-dev
```

Then build the benchmark, run:

``` shell
mkdir build
cd build
cmake ../ -DBUILD_BENCHMARK=1
make
ls -l gloo/benchmark/benchmark
```

## Benchmarking

The benchmark tool depends on Redis/Hiredis for rendezvous.
The benchmark tool for CUDA algorithms
obviously also depends on both CUDA and NCCL.

To run a benchmark:

1. Copy the benchmark tool to all participating machines

2. Start a Redis server on any host (either a client machine or one of
   the machines participating in the test). Note that Redis Cluster is **not** supported.

3. Determine some unique ID for the benchmark run (e.g. the `uuid`
   tool or some number).

4. On each machine, run (or pass `--help` for more options):

    ```
    ./benchmark \
      --size <number of machines> \
      --rank <index of this machine, starting at 0> \
      --redis-host <Redis host> \
      --redis-port <Redis port> \
      --prefix <unique identifier for this run> \
      --transport tcp \
      --elements <number of elements; -1 for a sweep> \
      --iteration-time 1s \
      allreduce_ring_chunked
    ```

Example output (running on 4 machines with a 40GbE network):

``` text
   elements   min (us)   p50 (us)   p99 (us)   max (us)    samples
          1        195        263        342        437       3921
          2        195        261        346        462       4039
          5        197        261        339        402       3963
         10        197        263        338        398       3749
         20        199        268        343        395       4146
         50        200        265        344        401       3889
        100        205        265        351        414       3645
        200        197        264        328        387       3960
        500        201        264        329        394       4274
       1000        200        267        330        380       3344
       2000        205        263        323        395       3682
       5000        240        335        424        460       3277
      10000        271        346        402        457       2721
      20000        283        358        392        428       2719
      50000        342        438        495        649       1654
     100000        413        487        669        799       1687
     200000       1113       1450       1837       2801        669
     500000       1099       1294       1665       1959        560
    1000000       1858       2286       2779       6100        320
    2000000       3546       3993       4364       4886        252
    5000000      10030      10608      11106      11628         92
```

## License

Gloo is BSD-licensed.
