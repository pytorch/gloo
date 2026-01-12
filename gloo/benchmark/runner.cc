/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "runner.h"

#include <cstdio>
#include <iomanip>
#include <iostream>

#include "gloo/barrier_all_to_one.h"
#include "gloo/broadcast_one_to_all.h"
#include "gloo/common/common.h"
#include "gloo/common/logging.h"
#include "gloo/common/utils.h"
#include "gloo/rendezvous/context.h"
#include "gloo/rendezvous/file_store.h"
#include "gloo/rendezvous/prefix_store.h"
#include "gloo/transport/device.h"

#if GLOO_USE_REDIS
#include "gloo/rendezvous/redis_store.h"
#endif

#if GLOO_USE_MPI
#include "gloo/mpi/context.h"
#endif

#if GLOO_HAVE_TRANSPORT_TCP
#include "gloo/transport/tcp/device.h"
#endif

#if GLOO_HAVE_TRANSPORT_TCP_TLS
#include "gloo/transport/tcp/tls/device.h"
#endif

#if GLOO_HAVE_TRANSPORT_IBVERBS
#include "gloo/transport/ibverbs/device.h"
#endif

namespace gloo {
namespace benchmark {

// Constant multiplier used to increase the iteration count
constexpr float kItersMultiplier = 1.2;
// Maximum number of iterations the benchmark will run when
// minimum time has been specified
constexpr int kMaxIterations = 1000000000;
// Maximum number of errors that can occur before the benchmark
// considers it to be too large and truncates them
constexpr int kMaxNumErrors = 100;

// Constants for formatting output
constexpr int kColWidthS = 11;
constexpr int kColWidthM = 13;
constexpr int kColWidthL = 19;
// Total width depends on number of columns on the table
constexpr int kTotalWidth = 6 * kColWidthS + kColWidthM + kColWidthL;
constexpr int kHeaderWidth = kTotalWidth / 2;

Runner::Runner(const options& options) : options_(options) {
#if GLOO_HAVE_TRANSPORT_TCP
  if (options_.transport == "tcp") {
    if (options_.tcpDevice.empty()) {
      transport::tcp::attr attr;
      transportDevices_.push_back(transport::tcp::CreateDevice(attr));
    } else {
      for (const auto& name : options_.tcpDevice) {
        transport::tcp::attr attr;
        attr.iface = name;
        transportDevices_.push_back(transport::tcp::CreateDevice(attr));
      }
    }
  }
#endif
#if GLOO_HAVE_TRANSPORT_TCP_TLS
  if (options_.transport == "tls") {
    if (options_.tcpDevice.empty()) {
      transport::tcp::attr attr;
      transportDevices_.push_back(
          transport::tcp::tls::CreateDevice(
              attr,
              options_.pkey,
              options_.cert,
              options_.caFile,
              options_.caPath));
    } else {
      for (const auto& name : options_.tcpDevice) {
        transport::tcp::attr attr;
        attr.iface = name;
        transportDevices_.push_back(
            transport::tcp::tls::CreateDevice(
                attr,
                options_.pkey,
                options_.cert,
                options_.caFile,
                options_.caPath));
      }
    }
  }
#endif
#if GLOO_HAVE_TRANSPORT_IBVERBS
  if (options_.transport == "ibverbs") {
    if (options_.ibverbsDevice.empty()) {
      transport::ibverbs::attr attr;
      attr.port = options_.ibverbsPort;
      attr.index = options_.ibverbsIndex;
      transportDevices_.push_back(transport::ibverbs::CreateDevice(attr));
    } else {
      for (const auto& name : options_.ibverbsDevice) {
        transport::ibverbs::attr attr;
        attr.name = name;
        attr.port = options_.ibverbsPort;
        attr.index = options_.ibverbsIndex;
        transportDevices_.push_back(transport::ibverbs::CreateDevice(attr));
      }
    }
  }
#endif

  GLOO_ENFORCE(
      !transportDevices_.empty(), "Unknown transport: ", options_.transport);

  // Spawn threads that run the actual benchmark loop
  for (auto i = 0; i < options_.threads; i++) {
    threads_.push_back(make_unique<RunnerThread>());
  }

#if GLOO_USE_REDIS
  if (!contextFactory_) {
    rendezvousRedis();
  }
#endif

#if GLOO_USE_MPI
  if (!contextFactory_) {
    rendezvousMPI();
  }
#endif

  if (!contextFactory_) {
    rendezvousFileSystem();
  }

  GLOO_ENFORCE(contextFactory_, "No means for rendezvous");

  // Create broadcast algorithm to synchronize between participants
  broadcast_.reset(
      new BroadcastOneToAll<long>(newContext(), {&broadcastValue_}, 1));

  // Create barrier for run-to-run synchronization
  barrier_.reset(new BarrierAllToOne(newContext()));
}

Runner::~Runner() {
  // Automatically delete rendezvous files after
  // benchmark is done running (if applicable)
  for (auto path : keyFilePaths_) {
    if (remove(path.c_str()) != 0) {
      std::cout << "Failed to delete rendezvous file at " << path;
      std::cout
          << " please delete manually before running the benchmark again.";
      std::cout << std::endl;
    }
  }

  // Reset algorithms and context factory such that all
  // shared_ptr's to contexts are destructed.
  // This is necessary so that all MPI common worlds are
  // destroyed before MPI_Finalize is called.
  barrier_.reset();
  broadcast_.reset();
  contextFactory_.reset();

#if GLOO_USE_MPI
  if (options_.mpi) {
    MPI_Finalize();
  }
#endif
}

#if GLOO_USE_REDIS
void Runner::rendezvousRedis() {
  // Don't rendezvous through Redis if the host is not set
  if (options_.redisHost.empty()) {
    return;
  }

  auto redisStore = std::make_shared<rendezvous::RedisStore>(
      options_.redisHost, options_.redisPort);
  auto prefixStore =
      std::make_shared<rendezvous::PrefixStore>(options_.prefix, redisStore);
  auto backingContext = std::make_shared<rendezvous::Context>(
      options_.contextRank, options_.contextSize);
  backingContext->connectFullMesh(prefixStore, transportDevices_.front());
  contextFactory_ =
      std::make_shared<rendezvous::ContextFactory>(backingContext);
}
#endif

#if GLOO_USE_MPI
void Runner::rendezvousMPI() {
  // Don't rendezvous using MPI if not started through mpirun
  if (!options_.mpi) {
    return;
  }

  auto rv = MPI_Init(nullptr, nullptr);
  GLOO_ENFORCE_EQ(rv, MPI_SUCCESS);
  MPI_Comm_rank(MPI_COMM_WORLD, &options_.contextRank);
  MPI_Comm_size(MPI_COMM_WORLD, &options_.contextSize);
  auto backingContext = std::make_shared<::gloo::mpi::Context>(MPI_COMM_WORLD);
  backingContext->connectFullMesh(transportDevices_.front());
  contextFactory_ =
      std::make_shared<rendezvous::ContextFactory>(backingContext);
}
#endif

void Runner::rendezvousFileSystem() {
  // Don't rendezvous using the file system if the shared path is not set
  if (options_.sharedPath.empty()) {
    return;
  }

  auto fileStore = std::make_shared<rendezvous::FileStore>(options_.sharedPath);
  auto prefixStore =
      std::make_shared<rendezvous::PrefixStore>(options_.prefix, fileStore);
  auto backingContext = std::make_shared<rendezvous::Context>(
      options_.contextRank, options_.contextSize);
  backingContext->connectFullMesh(prefixStore, transportDevices_.front());
  // After connectFullMesh is called, the rendezvous files will have been
  // generated so we need to fetch them from the FileStore
  keyFilePaths_ = fileStore->getAllKeyFilePaths();
  contextFactory_ =
      std::make_shared<rendezvous::ContextFactory>(backingContext);
}

long Runner::broadcast(long value) {
  // Set value to broadcast only on root.
  // Otherwise it can race with the actual broadcast
  // operation writing to the same memory location.
  if (options_.contextRank == 0) {
    broadcastValue_ = value;
  }
  broadcast_->run();
  return broadcastValue_;
}

std::shared_ptr<Context> Runner::newContext() {
  auto context = contextFactory_->makeContext(transportDevices_.front());
  return context;
}

template <typename T>
void Runner::run(BenchmarkFn<T>& fn) {
  printHeader();

  if (options_.elements > 0) {
    run(fn, options_.elements);
    checkErrors();
    printFooter();
    return;
  }

  // Run sweep over number of elements
  for (int i = 100; i <= 1000000; i *= 10) {
    std::vector<int> js = {i * 1, i * 2, i * 5};
    for (auto& j : js) {
      run(fn, j);
      // Check for errors after every iteration
      // checkErrors will exit the program if any errors were found
      checkErrors();
    }
  }
  printFooter();
}

template <typename T>
void Runner::run(BenchmarkFn<T>& fn, size_t n) {
  std::vector<std::unique_ptr<Benchmark<T>>> benchmarks;

  // Initialize one set of objects for every thread
  for (auto i = 0; i < options_.threads; i++) {
    auto context = contextFactory_->makeContext(
        transportDevices_[i % transportDevices_.size()]);
    context->base = options_.base;
    auto benchmark = fn(context);
    benchmark->initialize(n);

    // Switch pairs to sync mode if configured to do so
    if (options_.sync) {
      for (int j = 0; j < context->size; j++) {
        auto& pair = context->getPair(j);
        if (pair) {
          pair->setSync(true, options_.busyPoll);
        }
      }
    }

    // Verify correctness of initial run
    if (options_.verify) {
      benchmark->run();
      benchmark->verify(mismatchErrors_);
      barrier_->run();
    }

    benchmarks.push_back(std::move(benchmark));
  }

  // Create and run warmup jobs for every thread
  Samples warmupResults =
      createAndRun(benchmarks, options_.warmupIterationCount);

  // Iterations is the number of samples we will get.
  // If none specified, it will calculate an initial
  // iteration count based on the iteration time
  // (default 2s) and median time spent during warmup iters.
  auto iterations = options_.iterationCount;
  if (iterations <= 0) {
    GLOO_ENFORCE_GT(
        options_.minIterationTimeNanos, 0, "Iteration time must be positive");
    // Sort warmup iteration times
    Distribution warmup(warmupResults);
    // Broadcast duration of median iteration during warmup,
    // so all nodes agree on the number of iterations to run for.
    auto nanos = broadcast(warmup.percentile(0.5));
    iterations = std::max(1L, options_.minIterationTimeNanos / nanos);
  }

  Samples results;
  // Run the benchmark until results are significant enough to report
  while (1) {
    results = createAndRun(benchmarks, iterations);
    // If iteration count is explicitly specified by
    // user, report these results right away
    if (options_.iterationCount > 0) {
      break;
    }
    // Report these results if benchmark has run
    // for at least the minimum time
    auto totalNanos = results.sum() / options_.threads;
    if (totalNanos > options_.minIterationTimeNanos) {
      break;
    }
    // Stop if this run already used the maximum number of iterations
    if (iterations >= kMaxIterations) {
      break;
    }
    // Otherwise, increase the number of iterations again
    // and broadcast this value so all nodes agree on the
    // number of iterations to run for
    int nextIterations = static_cast<int>(kItersMultiplier * iterations);
    // When iterations is too small and multiplier has no effect,
    // just increment the number of iterations
    if (nextIterations <= iterations) {
      nextIterations++;
    }
    // Limit the number of iterations to kMaxIterations
    iterations = broadcast(std::min(nextIterations, kMaxIterations));
  }

  // Print results
  Distribution latency(results);
  printDistribution(n, sizeof(T), latency);
}

template <typename T>
Samples Runner::createAndRun(
    std::vector<std::unique_ptr<Benchmark<T>>>& benchmarks,
    int niters) {
  // Create jobs for every thread
  std::vector<std::unique_ptr<RunnerJob>> jobs;
  for (auto i = 0; i < options_.threads; i++) {
    auto& benchmark = benchmarks[i];
    auto fn = [&benchmark] { benchmark->run(); };
    auto job = make_unique<RunnerJob>(fn, niters);
    jobs.push_back(std::move(job));
  }

  // Start jobs on every thread (synchronized across processes)
  barrier_->run();
  for (auto i = 0; i < options_.threads; i++) {
    threads_[i]->run(jobs[i].get());
  }

  // Wait for completion
  for (auto i = 0; i < options_.threads; i++) {
    jobs[i]->wait();
  }

  // Synchronize again after running
  barrier_->run();

  // Merge results
  Samples samples;
  for (auto i = 0; i < options_.threads; i++) {
    samples.merge(jobs[i]->getSamples());
  }
  return samples;
}

void Runner::printHeader() {
  if (options_.contextRank != 0) {
    return;
  }
  std::string line = std::string(kTotalWidth + 2, '=');

  // ================================= ALGORITHM
  // =================================
  std::cout << line << std::endl;
  std::string algo = options_.benchmark;
  // Add offset to header width to center text
  int algoOffset = algo.length() / 2;
  // Print out algorithm name in upper case
  for (auto& c : algo) {
    c = std::toupper(c);
  }
  std::cout << std::right << std::setw(kHeaderWidth + algoOffset) << algo;
  std::cout << std::endl << std::endl;

  if (transportDevices_.size() == 1) {
    std::cout << std::left << std::setw(kColWidthM) << "Device:";
    std::cout << transportDevices_.front()->str() << std::endl;
  } else {
    std::cout << std::left << std::setw(kColWidthM) << "Devices:" << std::endl;
    for (const auto& device : transportDevices_) {
      std::cout << "  - " << device->str() << std::endl;
    }
  }

  std::cout << std::left << std::setw(kColWidthM) << "Options:";
  std::cout << "processes=" << options_.contextSize;
  std::cout << ", inputs=" << options_.inputs;
  std::cout << ", threads=" << options_.threads;
  if (options_.benchmark == "allreduce_bcube") {
    std::cout << ", base=" << options_.base;
  }
  if (options_.benchmark.compare(0, 5, "cuda_") == 0) {
    std::cout << ", gpudirect=";
    if (options_.transport == "ibverbs" && options_.gpuDirect) {
      std::cout << "yes";
    } else {
      std::cout << "no";
    }
  }
  std::cout << std::boolalpha;
  std::cout << ", verify=" << options_.verify;
  std::cout << std::endl << std::endl;

  // =============================== BENCHMARK RESULTS
  // ===============================
  std::cout << line << std::endl;
  // Section title
  std::string benchmarkTitle = "BENCHMARK RESULTS";
  // Add offset to header width to center text
  int offset = benchmarkTitle.length() / 2;
  std::cout << std::right << std::setw(kHeaderWidth + offset) << benchmarkTitle;
  std::cout << std::endl << std::endl;

  std::string suffix = "(us)";
  if (options_.showNanos) {
    suffix = "(ns)";
  }
  std::string bwSuffix = "(GB/s)";
  std::string sSuffix = "(B)";

  std::cout << std::right;
  std::cout << std::setw(kColWidthS) << ("size " + sSuffix);
  std::cout << std::setw(kColWidthS) << "elements";
  std::cout << std::setw(kColWidthS) << ("min " + suffix);
  std::cout << std::setw(kColWidthS) << ("p50 " + suffix);
  std::cout << std::setw(kColWidthS) << ("p99 " + suffix);
  std::cout << std::setw(kColWidthS) << ("max " + suffix);
  std::cout << std::setw(kColWidthL) << ("bandwidth " + bwSuffix);
  std::cout << std::setw(kColWidthM) << "iterations";
  std::cout << std::endl;
}

void Runner::printDistribution(
    size_t elements,
    size_t elementSize,
    const Distribution& latency) {
  // Only output results for one rank
  if (options_.contextRank != 0) {
    return;
  }

  auto div = 1000;
  if (options_.showNanos) {
    div = 1;
  }

  GLOO_ENFORCE_GE(latency.size(), 1, "No latency samples found");

  // Calculate total number of bytes (B) being sent
  auto bytes = elements * elementSize;
  auto totalBytes = bytes * latency.size();
  // Calculate total time (s) it took to send those bytes
  auto totalNanos = latency.sum() / options_.threads;
  auto totalSecs = totalNanos / 1e9f;
  // Calculate B/s being sent
  auto totalBytesPerSec = totalBytes / totalSecs;
  // Convert to GB/s
  auto totalGigaBytesPerSec = totalBytesPerSec / (1024 * 1024 * 1024);

  // Size and element columns display the size and element sent
  // per iteration and not total size and total elements
  std::cout << std::setw(kColWidthS) << bytes;
  std::cout << std::setw(kColWidthS) << elements;
  std::cout << std::setw(kColWidthS) << (latency.min() / div);
  std::cout << std::setw(kColWidthS) << (latency.percentile(0.50) / div);
  std::cout << std::setw(kColWidthS) << (latency.percentile(0.99) / div);
  std::cout << std::setw(kColWidthS) << (latency.max() / div);
  std::cout << std::fixed << std::setprecision(3);
  std::cout << std::setw(kColWidthL) << totalGigaBytesPerSec;
  std::cout << std::setw(kColWidthM) << latency.size();
  std::cout << std::endl;
}

void Runner::printVerifyHeader() {
  // Only print this for one rank
  if (options_.contextRank != 0) {
    return;
  }

  // Line separator
  std::string line = std::string(kTotalWidth + 2, '=');
  std::cout << std::endl << line << std::endl;

  // Section title
  std::string title = "VERIFY ERRORS";
  // Add offset to header width to center text
  int offset = title.length() / 2;
  std::cout << std::right << std::setw(kHeaderWidth + offset) << title;
  std::cout << std::endl << std::endl;
}

void Runner::printFooter() {
  // Only print this for one rank
  if (options_.contextRank != 0) {
    return;
  }

  std::string line = std::string(kTotalWidth + 2, '=');
  std::cout << std::endl << line << std::endl;
}

void Runner::checkErrors() {
  // Only check if that option has been set
  if (!options_.verify) {
    return;
  }
  // If there were no mismatches, don't print anything
  if (mismatchErrors_.empty()) {
    return;
  }
  // If there were mismatches, print them
  int size = mismatchErrors_.size();
  // Add barrier to prevent header from printing before benchmark results
  barrier_->run();
  printVerifyHeader();
  if (options_.contextRank == 0) {
    // Only print this stuff once
    if (size >= kMaxNumErrors && !options_.showAllErrors) {
      // If too many errors, inform user we will only be printing a subset
      std::cout << "Too many errors! Truncating to top 20 from each rank. ";
      std::cout << std::endl;
      std::cout << "Use the flag --show-all-errors to see all errors.";
      std::cout << std::endl << std::endl;
    }
  }
  if (size >= kMaxNumErrors && !options_.showAllErrors) {
    // Truncate errors if too many and user did not want to see all
    size = 20;
  }

  // Prints the errors from each rank in order
  // Since each rank is run on a different process, we occasionally have higher
  // ranks beginning to output their errors before lower ones causing overlaps.
  // This is confusing for the user so use a loop and a barrier at the beginning
  // of each iteration. This will force the processes to sync each time,
  // thus the output will be printed in the correct order.
  for (int i = 0; i < options_.contextSize; ++i) {
    barrier_->run();
    if (i != options_.contextRank) {
      // Skip if it is not current rank's turn
      continue;
    }
    for (int j = 0; j < size; ++j) {
      std::cout << mismatchErrors_[j] << std::endl;
    }
  }

  // Print footer and then exit program
  barrier_->run();
  printFooter();
  // Exit with error
  exit(1);
}

template void Runner::run(BenchmarkFn<char>& fn);
template void Runner::run(BenchmarkFn<char>& fn, size_t n);
template void Runner::run(BenchmarkFn<float>& fn);
template void Runner::run(BenchmarkFn<float>& fn, size_t n);
template void Runner::run(BenchmarkFn<float16>& fn);
template void Runner::run(BenchmarkFn<float16>& fn, size_t n);

RunnerThread::RunnerThread() : stop_(false), job_(nullptr) {
  thread_ = std::thread(&RunnerThread::spawn, this);
}

RunnerThread::~RunnerThread() {
  mutex_.lock();
  stop_ = true;
  mutex_.unlock();
  cond_.notify_one();
  thread_.join();
}

void RunnerThread::run(RunnerJob* job) {
  std::unique_lock<std::mutex> lock(mutex_);
  job_ = job;
  cond_.notify_one();
}

void RunnerThread::spawn() {
  setThreadName("gloo_bench");
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    while (job_ == nullptr) {
      cond_.wait(lock);
      if (stop_) {
        return;
      }
    }

    for (auto i = 0; i < job_->iterations_; i++) {
      Timer dt;
      job_->fn_();
      job_->samples_.add(dt);
    }

    job_->done();
    job_ = nullptr;
  }
}

} // namespace benchmark
} // namespace gloo
