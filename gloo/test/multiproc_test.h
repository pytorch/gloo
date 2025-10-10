/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gtest/gtest.h>
#include <semaphore.h>
#include <signal.h>

#include <chrono>
#include <string>

#include "gloo/common/enforce.h"
#include "gloo/rendezvous/context.h"
#include "gloo/rendezvous/file_store.h"
#include "gloo/test/base_test.h"

namespace gloo {
namespace test {

const int kExitWithIoException = 10;
const auto kMultiProcTimeout = std::chrono::milliseconds(3000);

class MultiProcTest : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  // Forks numRanks child processes that create a context of the defined
  // transport and run the provided lambda function.
  void spawnAsync(
      Transport transport,
      int numRanks,
      std::function<void(std::shared_ptr<Context>)> fn);

  // Waits on each forked child process.
  void wait();

  // Waits for the forked child process on the specified rank to change state.
  void waitProcess(int rank);

  // Kills the specified process using the supplied signal.
  void signalProcess(int rank, int signal);

  int getResult(int rank) {
    return workerResults_[rank];
  }

  // A single function that encapsulates spawnAsync to run the specified number
  // of child processes, waiting for their completion, and asserting correct
  // exit statuses.
  void spawn(
      Transport transport,
      int size,
      std::function<void(std::shared_ptr<Context>)> fn);

 private:
  // Creates a MultiProcWorker to run the specified lambda.
  int runWorker(
      Transport transport,
      int size,
      int rank,
      std::function<void(std::shared_ptr<Context>)> fn);

  std::string storePath_;
  std::string semaphoreName_;
  sem_t* semaphore_;

  // List of pid's for each forked child process.
  std::vector<pid_t> workers_;

  // Holds the exit statuses for each child process.
  std::vector<int> workerResults_;
};

class MultiProcWorker {
 public:
  explicit MultiProcWorker(
      const std::string& storePath,
      const std::string& semaphoreName) {
    store_ = std::unique_ptr<::gloo::rendezvous::Store>(
        new ::gloo::rendezvous::FileStore(storePath));
    semaphore_ = sem_open(semaphoreName.c_str(), 0);
    GLOO_ENFORCE_NE(semaphore_, (sem_t*)nullptr, strerror(errno));
  }

  ~MultiProcWorker() {
    sem_close(semaphore_);
  }

  void run(
      Transport transport,
      int size,
      int rank,
      std::function<void(std::shared_ptr<Context>)> fn) {
    auto context = std::make_shared<::gloo::rendezvous::Context>(rank, size);
    auto device = createDevice(transport);
    context->setTimeout(std::chrono::milliseconds(kMultiProcTimeout));
    context->connectFullMesh(store_, device);

    // Wait for all workers to be ready
    ringBarrier(context);

    device.reset();
    sem_post(semaphore_);
    fn(std::move(context));
  }

  void ringBarrier(std::shared_ptr<::gloo::rendezvous::Context>& context) {
    int sendScratch = 0;
    int recvScratch = 0;
    auto sendBuf =
        context->createUnboundBuffer(&sendScratch, sizeof(sendScratch));
    auto recvBuf =
        context->createUnboundBuffer(&recvScratch, sizeof(recvScratch));
    const auto leftRank = (context->size + context->rank - 1) % context->size;
    const auto rightRank = (context->rank + 1) % context->size;

    sendBuf->send(leftRank, 0);
    recvBuf->recv(rightRank, 0);
    sendBuf->waitSend();
    recvBuf->waitRecv();
  }

 protected:
  std::shared_ptr<::gloo::rendezvous::Store> store_;
  sem_t* semaphore_;
};

} // namespace test
} // namespace gloo
