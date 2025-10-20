/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/mpi/context.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "gloo/common/error.h"
#include "gloo/common/logging.h"
#include "gloo/transport/address.h"

namespace gloo {
namespace mpi {

static int MPICommSize(const MPI_Comm& comm) {
  int comm_size;
  auto error = MPI_Comm_size(comm, &comm_size);
  GLOO_ENFORCE(error == MPI_SUCCESS, "MPI_Comm_size: ", error);
  return comm_size;
}

static int MPICommRank(const MPI_Comm& comm) {
  int comm_rank;
  auto error = MPI_Comm_rank(comm, &comm_rank);
  GLOO_ENFORCE(error == MPI_SUCCESS, "MPI_Comm_rank: ", error);
  return comm_rank;
}

MPIScope::MPIScope() {
  auto rv = MPI_Init(nullptr, nullptr);
  GLOO_ENFORCE_EQ(rv, MPI_SUCCESS);
}

MPIScope::~MPIScope() {
  auto rv = MPI_Finalize();
  GLOO_ENFORCE_EQ(rv, MPI_SUCCESS);
}

namespace {

std::shared_ptr<MPIScope> getMPIScope() {
  static std::once_flag once;

  // Use weak pointer so that the initializer is destructed when the
  // last context referring to it is destructed, not when statics
  // are destructed on program termination.
  static std::weak_ptr<MPIScope> wptr;
  std::shared_ptr<MPIScope> sptr;

  // Create MPIScope only once
  std::call_once(once, [&]() {
    sptr = std::make_shared<MPIScope>();
    wptr = sptr;
  });

  // Create shared_ptr<MPIScope> from weak_ptr
  sptr = wptr.lock();
  GLOO_ENFORCE(sptr, "Cannot create MPI context after MPI_Finalize()");
  return sptr;
}

} // namespace

std::shared_ptr<Context> Context::createManaged() {
  auto mpiScope = getMPIScope();
  auto context = std::make_shared<Context>(MPI_COMM_WORLD);
  context->mpiScope_ = std::move(mpiScope);
  return context;
}

Context::Context(const MPI_Comm& comm)
    : ::gloo::Context(MPICommRank(comm), MPICommSize(comm)) {
  auto error = MPI_Comm_dup(comm, &comm_);
  GLOO_ENFORCE(error == MPI_SUCCESS, "MPI_Comm_dup: ", error);
}

Context::~Context() {
  MPI_Comm_free(&comm_);
}

void Context::connectFullMesh(std::shared_ptr<transport::Device>& dev) {
  std::vector<std::vector<char>> addresses(size);
  unsigned long maxLength = 0;
  int rv;

  // Create pair to connect to every other node in the collective
  auto transportContext = dev->createContext(rank, size);
  transportContext->setTimeout(getTimeout());
  for (int i = 0; i < size; i++) {
    if (i == rank) {
      continue;
    }

    auto& pair = transportContext->createPair(i);

    // Store address for pair for this rank
    auto address = pair->address().bytes();
    maxLength = std::max(maxLength, address.size());
    addresses[i] = std::move(address);
  }

  // Agree on maximum length so we can prepare buffers
  rv = MPI_Allreduce(
      MPI_IN_PLACE, &maxLength, 1, MPI_UNSIGNED_LONG, MPI_MAX, comm_);
  if (rv != MPI_SUCCESS) {
    GLOO_THROW_IO_EXCEPTION("MPI_Allreduce: ", rv);
  }

  // Prepare input and output
  std::vector<char> in(size * maxLength);
  std::vector<char> out(size * size * maxLength);
  for (int i = 0; i < size; i++) {
    if (i == rank) {
      continue;
    }

    auto& address = addresses[i];
    memcpy(in.data() + (i * maxLength), address.data(), address.size());
  }

  // Allgather to collect all addresses of all pairs
  rv = MPI_Allgather(
      in.data(), in.size(), MPI_BYTE, out.data(), in.size(), MPI_BYTE, comm_);
  if (rv != MPI_SUCCESS) {
    GLOO_THROW_IO_EXCEPTION("MPI_Allgather: ", rv);
  }

  // Connect every pair
  for (int i = 0; i < size; i++) {
    if (i == rank) {
      continue;
    }

    auto offset = (rank + i * size) * maxLength;
    std::vector<char> address(maxLength);
    memcpy(address.data(), out.data() + offset, maxLength);
    transportContext->getPair(i)->connect(address);
  }

  device_ = dev;
  transportContext_ = std::move(transportContext);
}

} // namespace mpi
} // namespace gloo
