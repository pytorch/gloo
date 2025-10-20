/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cassert>
#include <iostream>
#include <stdexcept>

#include "gloo/allreduce_ring.h"
#include "gloo/mpi/context.h"
#include "gloo/transport/tcp/device.h"

int main(int argc, char** argv) {
  auto rv = MPI_Init(&argc, &argv);
  if (rv != MPI_SUCCESS) {
    throw std::runtime_error("Failed to initialize MPI");
  }

  // We'll use the TCP transport in this example
  auto dev = gloo::transport::tcp::CreateDevice("localhost");

  // Use inner scope to force destruction of context and algorithm
  {
    // Create Gloo context from MPI communicator
    auto context = std::make_shared<gloo::mpi::Context>(MPI_COMM_WORLD);
    context->connectFullMesh(dev);

    // Create and run simple allreduce
    int rank = context->rank;
    gloo::AllreduceRing<int> allreduce(context, {&rank}, 1);
    allreduce.run();
    std::cout << "Result: " << rank << std::endl;
  }

  rv = MPI_Finalize();
  if (rv != MPI_SUCCESS) {
    throw std::runtime_error("Failed to Finalize MPI");
  }

  return 0;
}
