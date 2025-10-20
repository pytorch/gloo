/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cassert>
#include <iostream>

#include "gloo/allreduce_ring.h"
#include "gloo/mpi/context.h"
#include "gloo/transport/tcp/device.h"

int main(int /*argc*/, char** /*argv*/) {
  // We'll use the TCP transport in this example
  auto dev = gloo::transport::tcp::CreateDevice("localhost");

  // Create Gloo context and delegate management of MPI_Init/MPI_Finalize
  auto context = gloo::mpi::Context::createManaged();
  context->connectFullMesh(dev);

  // Create and run simple allreduce
  int rank = context->rank;
  gloo::AllreduceRing<int> allreduce(context, {&rank}, 1);
  allreduce.run();
  std::cout << "Result: " << rank << std::endl;

  return 0;
}
